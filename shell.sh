#!/bin/sh
# Interactive archive-root shell using the existing Docker SDB environment.
set -eu
TV_REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PYTHONPATH="$TV_REPO_DIR/src${PYTHONPATH:+:$PYTHONPATH}"
exec python3 "$TV_REPO_DIR/tools/archive_shell.py" "$@"
