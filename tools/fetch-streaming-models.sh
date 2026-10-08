#!/usr/bin/env bash
# fetch-streaming-models.sh — download the Moonshine streaming models OLAS 1.1
# needs, into ./models/<arch>-<lang>/ as symlinks to the SDK cache.
#
# Without arguments it fetches the three models the launcher can select:
#
#   medium-streaming-en   Medium Streaming English  (the English default)
#   small-streaming-es    Small Streaming Spanish   (the Spanish default)
#   small-streaming-en    Small Streaming English   (lower-CPU alternative)
#
# Add extra languages by name:
#
#   ./tools/fetch-streaming-models.sh fr de
#
# An extra language is fetched at the best streaming arch that exists for it;
# English is the only language with a Medium model today.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

log() { printf '\033[1;36m[olas]\033[0m %s\n' "$*" >&2; }
err() { printf '\033[1;31m[olas]\033[0m %s\n' "$*" >&2; }

VENV=""
for cand in "$SCRIPT_DIR/.venv" "$SCRIPT_DIR/../Olas-Linuxgtk/.venv"; do
    if [[ -x "$cand/bin/python" ]]; then VENV="$cand"; break; fi
done
if [[ -z "$VENV" ]]; then
    err "no .venv found; run ./build-gtk.sh first (it creates the venv)"
    exit 1
fi

LANGS=("$@")
if [[ ${#LANGS[@]} -eq 0 ]]; then LANGS=(en es); fi

# A bare run fetches every model the launcher offers. An explicit language
# list fetches only what was asked for, at that language's best streaming arch.
WANT_EXTRAS=1
[[ $# -gt 0 ]] && WANT_EXTRAS=0

WANT_EXTRAS="$WANT_EXTRAS" "$VENV/bin/python" - \
    "$SCRIPT_DIR/models" "${LANGS[@]}" <<'PY'
import os, sys, shutil
from pathlib import Path

MODELS = Path(sys.argv[1]).resolve()
LANGS = sys.argv[2:]
MODELS.mkdir(parents=True, exist_ok=True)

try:
    from moonshine_voice import get_model_for_language, ModelArch
except Exception as e:
    sys.exit(f"cannot import moonshine_voice: {e}")

# Per-language default: English uses Medium Streaming (the most complete
# English model); Spanish has no Medium model, so it uses Small.
PREFERENCE = {
    "en": ["MEDIUM_STREAMING", "SMALL_STREAMING"],
    "es": ["SMALL_STREAMING"],
}
DEFAULT = ["SMALL_STREAMING"]

PREFIX = {
    "MEDIUM_STREAMING": "medium-streaming",
    "SMALL_STREAMING":  "small-streaming",
    "TINY_STREAMING":   "tiny-streaming",
}

# On a bare run also grab the lower-CPU English alternative, so switching
# English down to Small needs no further downloads.
WANT_EXTRAS = os.environ.get("WANT_EXTRAS") == "1"
EXTRAS = [("en", "SMALL_STREAMING")] if WANT_EXTRAS else []


def fetch(lang, arch):
    last = None
    for call in (
        lambda: get_model_for_language(lang, wanted_model_arch=arch),
        lambda: get_model_for_language(lang, model_arch=arch),
        lambda: get_model_for_language(lang, arch),
    ):
        try:
            return call()
        except TypeError as e:
            last = e
            continue
    raise RuntimeError(f"get_model_for_language signature not understood: {last}")


def acquire(lang, arch_name):
    """Fetch one (language, arch) into ./models/<prefix>-<lang>/."""
    arch = getattr(ModelArch, arch_name, None)
    if arch is None:
        print(f"[olas]   {lang}/{arch_name}: build has no such arch", file=sys.stderr)
        return False

    dirname = f"{PREFIX[arch_name]}-{lang}"
    dest = MODELS / dirname
    if (dest / "streaming_config.json").is_file():
        print(f"[olas]   {dirname}: present")
        return True

    print(f"[olas]   {dirname}: downloading ({lang} {arch_name}) ...")
    try:
        src, _ = fetch(lang, arch)
    except Exception as e:
        print(f"[olas]   {dirname}: not available ({e})")
        return False

    src = Path(src).resolve()
    if not (src / "streaming_config.json").is_file():
        for sub in ("streaming", ""):
            cand = src / sub if sub else src
            if (cand / "streaming_config.json").is_file():
                src = cand
                break
        else:
            print(f"[olas]   {dirname}: streaming_config.json missing in {src}")
            return False

    if dest.is_symlink() or dest.is_file():
        dest.unlink()
    elif dest.exists():
        shutil.rmtree(dest)
    dest.symlink_to(src)
    print(f"[olas]   {dirname} -> {src}")
    return True


for lang in LANGS:
    for arch_name in PREFERENCE.get(lang, DEFAULT):
        acquire(lang, arch_name)
        break                      # first available arch is enough

for lang, arch_name in EXTRAS:
    acquire(lang, arch_name)
PY

log "models ready in $SCRIPT_DIR/models"
