#!/usr/bin/env python3
"""Interactive archive-root shell for the local Docker SDB setup."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from samsung_tv_root.archive_root import ArchiveRootError, probe
from samsung_tv_root.qn90f_shell import Qn90fRootShell, RootShellConfig
from samsung_tv_root.resources import payload_directory
from samsung_tv_root.root_agent import RootAgentConnection, RootAgentError
from samsung_tv_root.sdb import SdbClient, SdbError, route_callback_host


REPOSITORY = Path(__file__).resolve().parents[1]


def configure_payloads() -> None:
    release = (
        REPOSITORY
        / "build/tv-check/release/samsung-tv-root-0.0.9-macos-arm64"
        / "_internal/payloads"
    )
    for profile in ("common", "common31"):
        variable = f"SAMSUNG_TV_ROOT_{profile.upper()}_PAYLOAD_DIRECTORY"
        if variable in os.environ:
            continue
        if (payload_directory(profile) / "SamsungTvArchiveRoot.dll").is_file():
            continue
        if (release / profile / "SamsungTvArchiveRoot.dll").is_file():
            os.environ[variable] = str(release / profile)


async def run_shell(host: str, callback: str, port: int) -> int:
    async def session(agent: RootAgentConnection) -> int:
        identity = agent.identity
        print(
            f"Authenticated root: uid={identity.uid} gid={identity.gid}. "
            "Type exit or press Ctrl-D to disconnect.",
            flush=True,
        )
        return await Qn90fRootShell(
            RootShellConfig(tv_host=host, allowed_host=callback, port=port)
        ).run(agent)

    evidence = await probe(host, callback_host=callback, session_handler=session)
    destination = REPOSITORY / "build/tv-shell/last-session.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print("Shell closed; temporary files removed and system-file hashes verified.")
    return int(evidence["session_exit"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "host",
        nargs="?",
        default=os.environ.get("TV_HOST"),
        help="TV IPv4 address (or set TV_HOST)",
    )
    parser.add_argument(
        "--callback-host", help="Mac IPv4 address (auto-detected by default)"
    )
    parser.add_argument("--shell-port", type=int, default=22222)
    parser.add_argument(
        "--container", default=os.environ.get("SAMSUNG_TV_SDB_CONTAINER", "tizen-sdb")
    )
    arguments = parser.parse_args()
    if not arguments.host:
        parser.error("provide the TV host argument or set TV_HOST")
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        parser.error("run ./shell.sh directly in a terminal for an interactive shell")
    docker = shutil.which("docker")
    if docker is None:
        parser.error("Docker is not installed or is not on PATH")
    configure_payloads()
    os.environ["SAMSUNG_TV_SDB_CONTAINER"] = arguments.container
    adapter = REPOSITORY / "tools/sdb_docker.py"
    os.environ["SDB"] = str(adapter)
    callback = arguments.callback_host or route_callback_host(arguments.host)
    # Validate addresses and port before starting or connecting anything.
    RootShellConfig(arguments.host, callback, port=arguments.shell_port)
    subprocess.run(
        [docker, "start", arguments.container], check=True, stdout=subprocess.DEVNULL
    )
    client = SdbClient(adapter, arguments.host, timeout=15)
    try:
        print(
            f"Acquiring temporary root on {arguments.host} via Docker SDB...",
            flush=True,
        )
        return asyncio.run(run_shell(arguments.host, callback, arguments.shell_port))
    finally:
        try:
            client.disconnect()
        except SdbError as error:
            print(f"SDB disconnect failed: {error}", file=sys.stderr)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nShell interrupted.", file=sys.stderr)
        sys.exit(130)
    except (
        ArchiveRootError,
        RootAgentError,
        SdbError,
        OSError,
        ValueError,
        subprocess.SubprocessError,
    ) as error:
        print(f"shell: {error}", file=sys.stderr)
        sys.exit(1)
