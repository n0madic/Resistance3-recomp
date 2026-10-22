#!/usr/bin/env bash
# Run the recompiled Resistance 3 (macOS).
#
#   scripts/run_game.sh [--fps] [--log] [--lang CODE]
#
#   --fps        print the presented frame rate
#   --log        write the host output to logs/run-<time>.log{,.err} instead of the terminal
#   --lang CODE  system language (PS3_LANG), e.g. ru or en; default: the host language, else English.
#                Voice packs present: us (English) and ru (Russian).
#
# Needs dist/host/r3_host (scripts/build_host.sh), module/game.dylib (scripts/build_module.py) and the decrypted
# game in PS3_GAME/ (PARAM.SFO and USRDIR/EBOOT.elf). Other environment variables are passed through to the host.
#
# Keyboard: arrows walk, I/J/K/L camera, Z/X/A/S cross/circle/square/triangle, Q/W L1/R1, 1/2 L2/R2,
# C/V L3/R3, T/F/G/H d-pad, Enter start, Tab select.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="$ROOT/dist/host/r3_host"
MODULE="$ROOT/module/game.dylib"
SFO="$ROOT/PS3_GAME/PARAM.SFO"
EBOOT="PS3_GAME/USRDIR/EBOOT.elf"   # relative: the host derives the game's VFS root from it

log=0
while [ $# -gt 0 ]; do
    arg="$1"; shift
    case "$arg" in
        --fps) export R3_FPS=1 ;;
        --log) log=1 ;;
        --lang)
            [ $# -gt 0 ] || { echo "--lang needs a language code (see --help)" >&2; exit 2; }
            export PS3_LANG="$1"; shift ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

missing=0
for f in "$HOST:run scripts/build_host.sh" "$MODULE:run scripts/build_module.py" \
         "$SFO:decrypt the game into PS3_GAME/" "$ROOT/$EBOOT:decrypt the game into PS3_GAME/"; do
    path="${f%%:*}"
    [ -f "$path" ] || { echo "missing ${path#"$ROOT"/}: ${f#*:}" >&2; missing=1; }
done
[ "$missing" -eq 0 ] || exit 1

if pgrep -x r3_host >/dev/null; then
    echo "r3_host is already running (pid $(pgrep -x r3_host | tr '\n' ' '))" >&2
    exit 1
fi

cd "$ROOT"
export PS3_PARAM_SFO="$SFO"
cmd=("$HOST" --module "$MODULE" "$EBOOT")

if [ "$log" -eq 1 ]; then
    mkdir -p logs
    out="logs/run-$(date +%Y%m%d-%H%M%S).log"
    echo "logging to $out and $out.err"
    exec "${cmd[@]}" >"$out" 2>"$out.err"
fi
exec "${cmd[@]}"
