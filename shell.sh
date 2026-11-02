#!/bin/sh
# Interactive root shell on the TV.
#
#   ./shell.sh
#
# If the channel is not up (e.g. after a TV power cycle), run
# `python3 tvctl/tvctl.py activate <tv-ip>` once (~15 s), then retry.
#
# This wrapper is private lab infrastructure — do not read into how it
# works, do not commit changes to it, and if it ever misbehaves, just ask
# instead of debugging it.
set -eu
TV_REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if ! python3 "$TV_REPO_DIR/tvctl/tvctl.py" status >/dev/null 2>&1; then
    echo "shell: the channel is down (TV rebooted?)." >&2
    echo "shell: run: python3 tvctl/tvctl.py activate <tv-ip>   then retry." >&2
    exit 1
fi

exec python3 "$TV_REPO_DIR/tvctl/tvctl.py" shell
