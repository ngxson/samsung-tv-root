#!/bin/sh
# View this TV's graphical interface through the Docker SDB setup.
set -eu
TV_REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PYTHONPATH="$TV_REPO_DIR/src${PYTHONPATH:+:$PYTHONPATH}"
exec python3 "$TV_REPO_DIR/tools/tv_mirror.py" "$@"
