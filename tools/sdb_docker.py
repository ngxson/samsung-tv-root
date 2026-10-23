#!/usr/bin/env python3
"""Use Docker's SDB, copying host uploads into a private temporary directory."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


def main() -> int:
    docker = shutil.which("docker")
    if docker is None:
        sys.exit("docker is not on PATH")
    container = os.environ.get("SAMSUNG_TV_SDB_CONTAINER", "tizen-sdb")
    arguments = sys.argv[1:]
    command_index = 2 if arguments[:1] in (["-s"], ["--serial"]) else 0
    if len(arguments) > command_index and arguments[command_index] == "push":
        local_index = command_index + 1
        local = Path(arguments[local_index]).resolve(strict=True)
        directory = subprocess.check_output(
            [docker, "exec", container, "mktemp", "-d", "/tmp/tv-shell-XXXXXXXX"],
            text=True,
        ).strip()
        remote_local = directory + "/" + local.name
        try:
            subprocess.run(
                [docker, "cp", str(local), container + ":" + remote_local], check=True
            )
            arguments[local_index] = remote_local
            return subprocess.run(
                [docker, "exec", container, "sdb", *arguments]
            ).returncode
        finally:
            subprocess.run(
                [docker, "exec", container, "rm", "-rf", "--", directory], check=True
            )
    if len(arguments) > command_index and arguments[command_index] == "pull":
        sys.exit("This Docker adapter supports push and commands, not sdb pull.")
    os.execv(docker, [docker, "exec", container, "sdb", *arguments])
    return 1


if __name__ == "__main__":
    sys.exit(main())
