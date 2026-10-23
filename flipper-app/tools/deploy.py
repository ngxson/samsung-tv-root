#!/usr/bin/env python3
"""Build, exit an earlier Dummy Mic, upload, and verify USB re-enumeration."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

from micctl import find_port, request

ROOT = Path(__file__).resolve().parents[1]


def normal_port():
    ports = [
        p.device
        for p in list_ports.comports()
        if (p.serial_number or "").startswith("flip_")
    ]
    if len(ports) != 1:
        raise RuntimeError(
            f"Expected one normal Flipper serial port; found {len(ports)}"
        )
    return ports[0]


def wait_for_port(finder, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            return finder()
        except RuntimeError:
            time.sleep(0.2)
    return finder()  # Report the actual discovery error.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument(
        "--normal-launch",
        action="store_true",
        help="Disable the 15s enumeration watchdog",
    )
    args = parser.parse_args()
    if not args.no_build:
        subprocess.run([str(ROOT / "build.sh")], check=True, cwd=ROOT)
    sdk = Path(os.environ.get("UFBT_HOME", ROOT / ".ufbt")) / "current" / "scripts"
    if not (sdk / "flipper" / "storage.py").exists():
        raise RuntimeError(f"SDK storage helper missing: {sdk}; initialize uFBT first")
    sys.path.insert(0, str(sdk))
    from flipper.storage import FlipperStorage, FlipperStorageOperations

    try:
        running = find_port()
    except RuntimeError:
        running = None
    if running:
        print(request(running, "exit"))
    port = wait_for_port(normal_port)
    target = "/ext/apps/USB/dummy_mic.fap"
    with FlipperStorage(port) as storage:
        # Do not close another app; loader will reject launch if it is busy.
        FlipperStorageOperations(storage).recursive_send(
            target, str(ROOT / "dist" / "dummy_mic.fap"), False
        )
        argument = "" if args.normal_launch else " dev"
        storage.send(f'loader open "{target}"{argument}\r')
        # The old serial connection disappears intentionally. Success is verified
        # by pinging the NEW identity, never inferred from this disconnect.
        time.sleep(0.1)
    port = wait_for_port(find_port, 12)
    print(request(port, "ping"))
    print(request(port, "status"))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.CalledProcessError,
    ) as error:
        print(f"deploy: {error}", file=sys.stderr)
        raise SystemExit(1) from None
