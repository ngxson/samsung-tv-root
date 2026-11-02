#!/usr/bin/env bash
# Interactive key-code -> button-name mapping (see keymap.py). Ctrl+C saves and quits.
# Usage: ./keymap.sh [port]
set -euo pipefail
cd "$(dirname "$0")"

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)}"
if [ -z "$PORT" ]; then
    echo "No /dev/cu.usbmodem* found - is the board plugged in?" >&2
    exit 1
fi

# A killed idf.py monitor can leave the terminal raw (no Ctrl+C): reset it
if [ -t 0 ]; then
    stty sane
fi

PY="$(ls -d "$HOME"/.espressif/python_env/idf*_env/bin/python 2>/dev/null | tail -n1)"
exec "$PY" -I keymap.py "$PORT"
