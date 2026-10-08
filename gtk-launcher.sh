#!/usr/bin/env bash
# gtk-launcher.sh — run OLAS under a CPU quota.
#
# A per-thread sched_setaffinity() call inside the binary is best-effort:
# some ONNX Runtime builds reset their own affinity mask. A cgroup CPU
# quota is enforced by the kernel and cannot be bypassed by user code.
#
# The scope is transient and cleaned up when the app exits.
#
# CPU budget:
#   OLAS_CPU_QUOTA   percentage of one core; 400 = four cores' worth
#                    of CPU time. 0 disables the wrapper entirely.
#                    Default: 0 (disabled).
#
#                    The binary now bounds its own thread pool via
#                    MOONSHINE_ORT_SINGLE_THREAD and sched_setaffinity, so
#                    the cgroup quota is redundant and only adds latency
#                    jitter. Leave it at 0 unless you want a hard ceiling.
#
# Environment (also honoured):
#   OLAS_MOONSHINE_LANGS   comma list, e.g. "en,es" or "en"
#   OLAS_MOONSHINE_ARCHS   comma list, e.g. "5,4" (overrides defaults)
#   OLAS_CONFIG            path to the config file
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

log() { printf '\033[1;36m[olas]\033[0m %s\n' "$*" >&2; }
err() { printf '\033[1;31m[olas]\033[0m %s\n' "$*" >&2; }

# ---- locate binary ------------------------------------------------------
BIN="$SCRIPT_DIR/olas-gtk-1.1.bin"
[[ -x "$BIN" ]] || { err "$BIN not found; run ./build-gtk.sh first"; exit 1; }

# ---- locate venv --------------------------------------------------------
VENV=""
for cand in "$SCRIPT_DIR/.venv" "$SCRIPT_DIR/../Olas-Linuxgtk/.venv"; do
    if [[ -x "$cand/bin/python" ]]; then
        VENV="$cand"
        break
    fi
done

# ---- locate config ------------------------------------------------------
CONF_FILE="$SCRIPT_DIR/olas-1.1.conf"
[[ -f "$CONF_FILE" ]] || CONF_FILE=""

# ---- per-language default arch ------------------------------------------
# OLAS 1.1 has two modes, defined by the English model:
#
#   Normal mode (default)  Medium English + Small Spanish.
#                          English gets 3 cores, Spanish 1. More accurate.
#   Potato mode            Small English + Small Spanish.
#                          One core per language, 2 total. Ultralight.
#
# Spanish always uses Small Streaming: no Medium Spanish model exists.
#
# Override with OLAS_MOONSHINE_ARCHS, or the friendly OLAS_MODE below.
#     OLAS_MODE=potato ./gtk-launcher.sh     # both Small, lowest CPU
#     OLAS_MODE=normal ./gtk-launcher.sh     # the default, explicit
#
default_arch_for() {
    case "$1" in
        en) echo "MEDIUM_STREAMING 5 medium-streaming" ;;
        *)  echo "SMALL_STREAMING  4 small-streaming"  ;;
    esac
}

# Model directory prefix for an architecture number, so an arch override also
# selects the matching weights instead of loading Medium weights out of the
# Small directory.
model_prefix_for_arch() {
    case "$1" in
        2) echo "tiny-streaming"   ;;
        4) echo "small-streaming"  ;;
        5) echo "medium-streaming" ;;
        *) echo "small-streaming"  ;;
    esac
}

arch_name_for_num() {
    case "$1" in
        2) echo "TINY_STREAMING"   ;;
        4) echo "SMALL_STREAMING"  ;;
        5) echo "MEDIUM_STREAMING" ;;
        *) echo "SMALL_STREAMING"  ;;
    esac
}

# ---- languages ----------------------------------------------------------
if [[ -n "${OLAS_MOONSHINE_LANGS:-}" ]]; then
    LANGS="$OLAS_MOONSHINE_LANGS"
    LANG_PASS=(--language "$LANGS")
    LANGS="$(echo "$LANGS" | tr -d ' ')"
    IFS=',' read -r -a LANG_ARR <<< "$LANGS"
else
    LANG_PASS=()
    LANG_ARR=(en es)
fi

# ---- mode ---------------------------------------------------------------
# OLAS_MODE is the friendly spelling of the English model choice; explicit
# OLAS_MOONSHINE_ARCHS still wins if both are given.
if [[ -n "${OLAS_MODE:-}" && -z "${OLAS_MOONSHINE_ARCHS:-}" ]]; then
    case "$(echo "$OLAS_MODE" | tr '[:upper:]' '[:lower:]')" in
        potato|light|ultralight|small) OLAS_MOONSHINE_ARCHS="4,4" ;;
        normal|medium|default)         OLAS_MOONSHINE_ARCHS="5,4" ;;
        *) err "OLAS_MODE='$OLAS_MODE' not recognised (use 'normal' or 'potato')"
           exit 1 ;;
    esac
fi

# ---- resolve model path + arch per language -----------------------------
# OLAS_MOONSHINE_ARCHS, if set, wins per language: it selects both the arch
# number and the matching model directory, so `OLAS_MOONSHINE_ARCHS=5,4`
# loads medium-streaming-en for English and small-streaming-es for Spanish.
ARCH_OVERRIDE=()
if [[ -n "${OLAS_MOONSHINE_ARCHS:-}" ]]; then
    IFS=',' read -r -a ARCH_OVERRIDE <<< "$(echo "$OLAS_MOONSHINE_ARCHS" | tr -d ' ')"
fi

MODELS=()
ARCHES=()

for idx in "${!LANG_ARR[@]}"; do
    L="${LANG_ARR[$idx]}"
    read -r ARCH_NAME ARCH_NUM MODEL_PREFIX <<< "$(default_arch_for "$L")"

    # An explicit override for this position replaces the default arch.
    if [[ -n "${ARCH_OVERRIDE[$idx]:-}" ]]; then
        ARCH_NUM="${ARCH_OVERRIDE[$idx]}"
        ARCH_NAME="$(arch_name_for_num "$ARCH_NUM")"
        MODEL_PREFIX="$(model_prefix_for_arch "$ARCH_NUM")"
    fi

    LOCAL="$SCRIPT_DIR/models/${MODEL_PREFIX}-${L}"
    if [[ -f "$LOCAL/streaming_config.json" ]]; then
        P="$LOCAL"
        A="$ARCH_NUM"
    else
        if [[ -z "$VENV" ]]; then
            err "$LOCAL not found and no venv available"
            err "run ./build-gtk.sh to download models"
            exit 1
        fi
        if ! OUT="$("$VENV/bin/python" - "$L" "$ARCH_NAME" <<'PY'
import sys
from moonshine_voice import get_model_for_language, ModelArch

lang, arch_name = sys.argv[1], sys.argv[2]
arch = getattr(ModelArch, arch_name, None)
if arch is None:
    sys.exit(f"unknown arch '{arch_name}' in this moonshine-voice build")

last = None
for call in (
    lambda: get_model_for_language(lang, wanted_model_arch=arch),
    lambda: get_model_for_language(lang, model_arch=arch),
    lambda: get_model_for_language(lang, arch),
):
    try:
        p, a = call()
        val = getattr(a, "value", a)
        print(f"{p}\t{val}")
        sys.exit(0)
    except TypeError as e:
        last = e
        continue
sys.exit(f"could not resolve model for '{lang}' arch '{arch_name}': {last}")
PY
)"; then
            err "failed to resolve model for $L"
            err "expected: $LOCAL"
            err "run ./build-gtk.sh to download it"
            exit 1
        fi
        IFS=$'\t' read -r P A <<< "$OUT"
    fi

    MODELS+=("$P")
    ARCHES+=("$A")
    log "  $L -> arch=$A  $P"
done

# ---- arch list ----------------------------------------------------------
# Already resolved per language above (defaults, or OLAS_MOONSHINE_ARCHS).
ARCHS="$(IFS=','; echo "${ARCHES[*]}")"
if [[ -n "${OLAS_MOONSHINE_ARCHS:-}" ]]; then
    log "arch override from env: $ARCHS"
fi

MODEL_CSV="$(IFS=','; echo "${MODELS[*]}")"

# ---- config -------------------------------------------------------------
CONF_PASS=()
if [[ -n "${OLAS_CONFIG:-}" ]]; then
    CONF_PASS=(--config "$OLAS_CONFIG")
elif [[ -n "$CONF_FILE" ]]; then
    CONF_PASS=(--config "$CONF_FILE")
fi

log "binary:  $BIN"
log "archs:   $ARCHS"
log "models:  $MODEL_CSV"
[[ -n "$CONF_FILE" ]] && log "config:  $CONF_FILE"

# ---- CPU quota ----------------------------------------------------------
# Percentage of one CPU core. 400% = four cores' worth of CPU time. The
# kernel enforces this at the cgroup level, so ONNX Runtime's thread
# pool cannot escape it no matter how it configures itself.
QUOTA="${OLAS_CPU_QUOTA:-0}"

# Build the command array once so we can reuse it for both paths.
CMD=( "$BIN" "${LANG_PASS[@]}" --arch "$ARCHS" --model "$MODEL_CSV" \
      "${CONF_PASS[@]}" "$@" )

if [[ "$QUOTA" == "0" ]]; then
    log "CPU quota disabled (OLAS_CPU_QUOTA=0)"
    exec "${CMD[@]}"
fi

if ! command -v systemd-run >/dev/null 2>&1; then
    err "systemd-run not found; cannot enforce a CPU quota"
    err "install systemd (or set OLAS_CPU_QUOTA=0 to run unpinned)"
    exit 1
fi

# Verify the user's systemd session is reachable. If not (e.g. running
# from a bare TTY without a user manager), fall back to a clear error
# rather than silently running unpinned.
if ! systemctl --user is-system-running >/dev/null 2>&1 \
   && ! systemctl --user status >/dev/null 2>&1; then
    err "no user systemd session; cannot enforce a CPU quota"
    err "start a graphical session, or set OLAS_CPU_QUOTA=0 to run unpinned"
    exit 1
fi

log "CPU quota: ${QUOTA}% (via systemd-run --user --scope)"

exec systemd-run --user --scope --collect -q \
    -p "CPUQuota=${QUOTA}%" \
    -p "CPUWeight=20" \
    -- "${CMD[@]}"
