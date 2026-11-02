#!/usr/bin/env bash
# Print the ESP32 serial log and save it to logs/ (Ctrl+C to quit).
# Usage: ./monitor.sh [-r] [port]
#   -r    reset the board first, to get the boot log
#   port  defaults to the first /dev/cu.usbmodem*
set -euo pipefail
cd "$(dirname "$0")"

RESET=0
if [ "${1:-}" = "-r" ]; then
    RESET=1
    shift
fi

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)}"
if [ -z "$PORT" ]; then
    echo "No /dev/cu.usbmodem* found - is the board plugged in?" >&2
    exit 1
fi

PY="$(ls -d "$HOME"/.espressif/python_env/idf*_env/bin/python 2>/dev/null | tail -n1)"
# A killed idf.py monitor can leave the terminal raw (no Ctrl+C): reset it
if [ -t 0 ]; then
    stty sane
fi

mkdir -p logs
LOG="logs/monitor-$(date +%Y%m%d-%H%M%S).log"
ln -sf "$(basename "$LOG")" logs/latest.log
echo "port=$PORT log=$LOG (Ctrl+C to quit)" >&2

exec "$PY" -I -u -c '
import os, signal, sys, time, serial
port, log, reset = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
stop = False
def on_signal(*_):
    global stop
    if stop:
        os._exit(130)  # second Ctrl+C: quit immediately
    stop = True
# Install handlers explicitly: SIGINT may be inherited as ignored
for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, on_signal)
try:
    # exclusive: a second monitor/keymap/flash must fail loudly, not share the port
    s = serial.Serial(port, 115200, timeout=0.2, exclusive=True)
except serial.SerialException as e:
    print(f"Cannot open {port}: {e}\nIs another monitor.sh / keymap.sh / idf.py using it?", file=sys.stderr)
    sys.exit(1)
if reset:
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
with open(log, "ab") as f:
    while not stop:
        d = s.read(4096)
        if d:
            sys.stdout.buffer.write(d); sys.stdout.flush()
            f.write(d); f.flush()
s.close()
print("\n[monitor stopped]", file=sys.stderr)
' "$PORT" "$LOG" "$RESET"
