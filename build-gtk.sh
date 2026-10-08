#!/usr/bin/env bash
# build-gtk.sh — build OLAS from a clean tree.
#
# First run does everything:
#   1. creates ./.venv (with --copies, avoiding a Python 3.13/3.14 venv bug)
#   2. pip-installs moonshine-voice into the venv
#   3. downloads the Moonshine C++ SDK from GitHub Releases into
#      ./moonshine-voice/ (include/ + lib/)
#   4. downloads models into ./models/:
#        medium-streaming-en   (Medium Streaming, English — the default)
#        small-streaming-es    (Small Streaming, Spanish)
#        small-streaming-en    (Small Streaming, English — lower CPU option)
#   5. builds ./olas-gtk-<ver>.bin against the local SDK.
#
# ./build-gtk.sh cfgtest works before any of the above — it needs only g++.
#
# Environment:
#   OLAS_PYTHON   interpreter to build the venv with (default: python3).
#                 Useful on Arch/CachyOS when the system Python is too new
#                 for the native wheels moonshine-voice depends on:
#                   OLAS_PYTHON=/usr/bin/python3.12 ./build-gtk.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

log()  { printf '\033[1;36m[olas]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[olas]\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[1;31m[olas]\033[0m %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# cfgtest — compile and run the config-parser test without GTK or the SDK.
# Must come first: it needs neither a venv nor a display.
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "cfgtest" ]]; then
    SRC="src/olas-gtk-1.1.cpp"
    [[ -f "$SRC" ]] || { err "source not found: $SRC"; exit 1; }

    BLOCK_END=$(grep -n '^static OlasConfig g_ocfg;' "$SRC" | head -1 | cut -d: -f1)
    BLOCK_START=$(grep -n '^struct OlasConfig {' "$SRC" | head -1 | cut -d: -f1)
    if [[ -z "$BLOCK_START" || -z "$BLOCK_END" ]]; then
        err "could not locate the config block in $SRC"
        err "  (grep patterns: 'struct OlasConfig {' and 'static OlasConfig g_ocfg;')"
        exit 1
    fi

    INC=/tmp/olas_cfgblock.h
    {
        echo '#pragma once'
        echo '#include <string>'
        echo '#include <vector>'
        echo '#include <map>'
        echo '#include <fstream>'
        echo '#include <cstdlib>'
        echo '#include <cctype>'
        echo '#include <algorithm>'
        sed -n "${BLOCK_START},${BLOCK_END}p" "$SRC"
    } > "$INC"

    TEST_SRC="tools/config_test.cpp"
    [[ -f "$TEST_SRC" ]] || { err "no config test source at $TEST_SRC"; exit 1; }

    log "config test (block: lines $BLOCK_START-$BLOCK_END of $SRC)"
    g++ -std=c++17 -O2 -Wall -Wextra -DCFG_BLOCK=\"$INC\" \
        "$TEST_SRC" -o "${TEST_SRC%.cpp}"
    log "built: $SCRIPT_DIR/${TEST_SRC%.cpp}  (run it from this directory)"
    exit 0
fi

# ---------------------------------------------------------------------------
# Clean up dead symlinks from earlier tree layouts.
# ---------------------------------------------------------------------------
for link in moonshine-voice models; do
    if [[ -L "$link" && ! -e "$link" ]]; then
        warn "removing broken symlink: $link"
        rm -f "$link"
    fi
done

SRC="src/olas-gtk-1.1.cpp"
[[ -f "$SRC" ]] || { err "source not found: $SCRIPT_DIR/$SRC"; exit 1; }
BIN="$(basename "${SRC%.cpp}").bin"
log "source: $SRC  ->  $BIN"

# ---------------------------------------------------------------------------
# Virtual environment.
#
# --copies comes first on purpose. Plain `python3 -m venv` writes
# .venv/bin/python3 as a symlink to a path that resolves relative to the
# venv, and on Python 3.13/3.14 on Arch this leaves a dangling link, so
# ensurepip's subprocess dies with "No such file or directory:
# .venv/bin/python3". --copies sidesteps it by copying the real binary.
# ---------------------------------------------------------------------------
VENV="$SCRIPT_DIR/.venv"

create_venv() {
    local target="$1"
    local py="${OLAS_PYTHON:-python3}"

    if ! "$py" -c 'import venv' >/dev/null 2>&1; then
        err "$py has no working 'venv' module"
        return 1
    fi

    # 1. --copies: the reliable path on Arch/CachyOS Python 3.13+.
    rm -rf "$target"
    if "$py" -m venv --copies "$target" >/dev/null 2>&1 \
       && [[ -x "$target/bin/python" ]] \
       && "$target/bin/python" -c 'import sys' >/dev/null 2>&1; then
        return 0
    fi

    # 2. Plain venv, for the rare build where --copies misbehaves.
    rm -rf "$target"
    if "$py" -m venv "$target" >/dev/null 2>&1 \
       && [[ -x "$target/bin/python" ]] \
       && "$target/bin/python" -c 'import sys' >/dev/null 2>&1; then
        return 0
    fi

    # 3. venv without pip, then bootstrap pip manually.
    rm -rf "$target"
    if "$py" -m venv --copies --without-pip "$target" >/dev/null 2>&1 \
       && [[ -x "$target/bin/python" ]] \
       && "$target/bin/python" -m ensurepip --upgrade >/dev/null 2>&1; then
        return 0
    fi

    # 4. virtualenv, if it happens to be installed.
    if command -v virtualenv >/dev/null 2>&1; then
        rm -rf "$target"
        if virtualenv -p "$py" "$target" >/dev/null 2>&1 \
           && [[ -x "$target/bin/python" ]]; then
            return 0
        fi
    fi

    rm -rf "$target"
    return 1
}

if [[ ! -x "$VENV/bin/python" ]] \
   || ! "$VENV/bin/python" -c 'import sys' >/dev/null 2>&1; then
    log "creating venv at $VENV"
    if ! create_venv "$VENV"; then
        err "could not create a working venv after 4 attempts."
        err "diagnose with:"
        err "  python3 -m venv --copies /tmp/t && /tmp/t/bin/python -V"
        err "or force an interpreter: OLAS_PYTHON=/usr/bin/python3.13 ./build-gtk.sh"
        exit 1
    fi
    "$VENV/bin/pip" install --upgrade pip wheel >/dev/null 2>&1 || true
fi
PY="$VENV/bin/python"
log "venv: $VENV ($("$PY" --version 2>&1))"

# ---------------------------------------------------------------------------
# moonshine-voice Python package.
# ---------------------------------------------------------------------------
if ! "$PY" -c 'import moonshine_voice' >/dev/null 2>&1; then
    log "installing moonshine-voice from PyPI"
    if ! "$VENV/bin/pip" install moonshine-voice; then
        err "pip install moonshine-voice failed."
        err ""
        err "If the error mentions 'onnxruntime' or 'no matching distribution',"
        err "it means this Python version ($("$PY" --version 2>&1 | awk '{print $2}'))"
        err "is too new for the native wheels moonshine-voice depends on."
        err "Install an older interpreter and retry:"
        err "  sudo pacman -S python312"
        err "  OLAS_PYTHON=/usr/bin/python3.12 ./build-gtk.sh"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Moonshine C++ SDK from GitHub Releases.
#
# The PyPI wheel only ships the Python bindings; the C++ headers and
# libmoonshine.so come from a separate prebuilt archive published on
# GitHub Releases as moonshine-voice-<platform>.tar.gz. The archive's
# top-level directory is stripped so include/ and lib/ land directly
# under ./moonshine-voice/.
# ---------------------------------------------------------------------------
MOONSHINE_DIR="$SCRIPT_DIR/moonshine-voice"

if [[ ! -f "$MOONSHINE_DIR/include/moonshine-cpp.h" ]]; then
    log "downloading Moonshine C++ SDK from GitHub Releases"

    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64)         SDK_ARCH="x86_64" ;;
        aarch64|arm64)  SDK_ARCH="arm64"  ;;
        *) err "unsupported architecture: $ARCH"; exit 1 ;;
    esac

    SDK_URL="https://github.com/moonshine-ai/moonshine/releases/latest/download/moonshine-voice-linux-${SDK_ARCH}.tar.gz"
    SDK_TARBALL="/tmp/moonshine-voice-linux-${SDK_ARCH}.tar.gz"

    if ! curl -fL --retry 3 -o "$SDK_TARBALL" "$SDK_URL"; then
        err "download failed: $SDK_URL"
        err "grab the archive manually from:"
        err "  https://github.com/moonshine-ai/moonshine/releases"
        err "then extract into $MOONSHINE_DIR so that include/ and lib/ are direct children."
        exit 1
    fi

    rm -rf "$MOONSHINE_DIR"
    mkdir -p "$MOONSHINE_DIR"
    if ! tar xzf "$SDK_TARBALL" -C "$MOONSHINE_DIR" --strip-components=1; then
        err "extraction failed; archive may be corrupt"
        rm -f "$SDK_TARBALL"
        exit 1
    fi
    rm -f "$SDK_TARBALL"

    if [[ ! -f "$MOONSHINE_DIR/include/moonshine-cpp.h" ]]; then
        err "SDK extracted but moonshine-cpp.h is not where expected."
        err "Inspect $MOONSHINE_DIR and check the archive layout."
        exit 1
    fi

    log "SDK ready: $MOONSHINE_DIR"
else
    log "SDK present: $MOONSHINE_DIR"
fi

# ---------------------------------------------------------------------------
# Models.
#
# Three models are fetched so every configuration is available offline:
#
#   medium-streaming-en  Medium Streaming English  (the English default)
#   small-streaming-es   Small Streaming Spanish  (the only Spanish model)
#   small-streaming-en   Small Streaming English  (lower-CPU alternative)
#
# The launcher defaults to Medium English + Small Spanish. English can be
# switched down to Small at run time without re-downloading:
#
#   OLAS_MOONSHINE_ARCHS=4,4 ./gtk-launcher.sh
#
# The Python package knows the canonical download URLs; we ask it to fetch
# each model and symlink it into ./models/<name>-<lang>/.
# ---------------------------------------------------------------------------
mkdir -p models

log "checking models (medium en, small en+es)"
"$PY" - "$SCRIPT_DIR/models" <<'PY'
import sys, shutil
from pathlib import Path

MODELS = Path(sys.argv[1]).resolve()
MODELS.mkdir(parents=True, exist_ok=True)

try:
    from moonshine_voice import get_model_for_language, ModelArch
except Exception as e:
    sys.exit(f"cannot import moonshine_voice: {e}")

TARGETS = [
    ("en", "MEDIUM_STREAMING", "medium-streaming-en"),
    ("es", "SMALL_STREAMING",  "small-streaming-es"),
    ("en", "SMALL_STREAMING",  "small-streaming-en"),
]

def fetch(lang, arch):
    """Try the handful of signatures the SDK has shipped with."""
    last = None
    for call in (
        lambda: get_model_for_language(lang, wanted_model_arch=arch),
        lambda: get_model_for_language(lang, model_arch=arch),
        lambda: get_model_for_language(lang, arch),
    ):
        try:
            p, a = call()
            return Path(p), a
        except TypeError as e:
            last = e
            continue
    raise RuntimeError(f"get_model_for_language signature not understood: {last}")

for lang, arch_name, dirname in TARGETS:
    dest = MODELS / dirname
    if (dest / "streaming_config.json").is_file():
        print(f"[olas]   {dirname}: present")
        continue

    arch = getattr(ModelArch, arch_name, None)
    if arch is None:
        sys.exit(f"ModelArch has no member {arch_name}")

    print(f"[olas]   {dirname}: downloading ({lang} {arch_name}) ...")
    try:
        src, resolved = fetch(lang, arch)
    except Exception as e:
        sys.exit(f"download failed for {lang}/{arch_name}: {e}")

    src = src.resolve()
    if not src.is_dir():
        sys.exit(f"unexpected model path (not a directory): {src}")
    if not (src / "streaming_config.json").is_file():
        # Some SDK builds return the parent; try common subdirs.
        for sub in ("streaming", ""):
            cand = src / sub if sub else src
            if (cand / "streaming_config.json").is_file():
                src = cand
                break
        else:
            sys.exit(f"streaming_config.json missing in {src}")

    if dest.exists() or dest.is_symlink():
        if dest.is_symlink() or dest.is_file():
            dest.unlink()
        else:
            shutil.rmtree(dest)
    dest.symlink_to(src)
    print(f"[olas]   {dirname} -> {src}")
PY

# ---------------------------------------------------------------------------
# Config file presence (informational only).
# ---------------------------------------------------------------------------
CONF=""
for c in olas-1.1.conf; do
    [[ -f "$c" ]] && { CONF="$c"; break; }
done
if [[ -n "$CONF" ]]; then
    log "config: $CONF"
else
    warn "no olas-*.conf found; built-in defaults apply"
fi

# ---------------------------------------------------------------------------
# Build dependencies.
# ---------------------------------------------------------------------------
if ! pkg-config --exists gtk4; then
    err "gtk4 development package not found"
    err "  Arch / CachyOS : sudo pacman -S gtk4 pkgconf"
    err "  Debian / Ubuntu: sudo apt install libgtk-4-dev pkg-config"
    err "  Fedora         : sudo dnf install gtk4-devel pkgconf-pkg-config"
    err "  (./build-gtk.sh cfgtest works without it)"
    exit 1
fi

if ! pkg-config --exists libpulse-simple; then
    err "libpulse development package not found"
    err "  Arch / CachyOS : sudo pacman -S libpulse"
    err "  Debian / Ubuntu: sudo apt install libpulse-dev"
    err "  Fedora         : sudo dnf install pulseaudio-libs-devel"
    exit 1
fi

# ---------------------------------------------------------------------------
# Compile.
# ---------------------------------------------------------------------------
log "compiling $SRC"
# v1.1 is split across src/: the main TU plus the hardware probe and the
# resource planner. Keep -march=native: the probe and planner are pure CPU
# code and gain from it; the Moonshine SDK is a shared object and unaffected.
g++ "$SRC" \
    "$SCRIPT_DIR/src/system_probe.cpp" \
    "$SCRIPT_DIR/src/resource_plan.cpp" \
    -O3 -march=native -flto -DNDEBUG \
    -std=c++17 -Wall -Wextra \
    -I"$SCRIPT_DIR/src" \
    -I"$MOONSHINE_DIR/include" \
    -L"$MOONSHINE_DIR/lib" \
    -lmoonshine \
    -Wl,-rpath,"$MOONSHINE_DIR/lib" \
    -lpulse -lpulse-simple -lpthread -lm \
    $(pkg-config --cflags --libs gtk4) \
    -o "$BIN"

log "built: $SCRIPT_DIR/$BIN"
log "run:   ./gtk-launcher.sh"
