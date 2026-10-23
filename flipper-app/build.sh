#!/bin/sh
set -eu
cd "$(dirname "$0")"
export UFBT_HOME="${UFBT_HOME:-$PWD/.ufbt}"
exec ufbt "$@"
