#!/usr/bin/env python3
"""Send one line command to the running Dummy USB Mic, or watch its counters."""

import argparse
import json
import sys
import time

import serial
from serial.tools import list_ports


def find_port():
    ports = [
        p.device
        for p in list_ports.comports()
        if p.vid == 0x1209 and p.pid == 0x0001 and p.serial_number == "dummy_mic_001"
    ]
    if len(ports) != 1:
        raise RuntimeError(
            f"Expected one running Dummy USB Mic, found {len(ports)}. "
            "Launch the app, or specify --port."
        )
    return ports[0]


def request(port, command, timeout=3.0):
    if not command or any(ord(c) < 32 or ord(c) > 126 for c in command):
        raise ValueError("Command must be one nonempty ASCII line")
    if len(command) >= 64:
        raise ValueError("Command must be shorter than 64 bytes")
    with serial.Serial(port, 115200, timeout=0.1, write_timeout=timeout) as connection:
        connection.reset_input_buffer()
        # A leading newline resynchronizes any partial command left by a client.
        connection.write(b"\n" + command.encode("ascii") + b"\n")
        deadline = time.monotonic() + timeout
        pending = bytearray()
        while time.monotonic() < deadline:
            # Do not wait for 512 bytes: exit/reconnect deliberately removes the
            # device immediately after acknowledging the command.
            pending.extend(connection.read(max(1, min(connection.in_waiting, 512))))
            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending = bytearray(rest)
                reply = line.decode("ascii", errors="replace").strip()
                if reply == "ERR line discarded":
                    continue  # Response to the leading resynchronization newline.
                if reply.startswith("ERR "):
                    raise RuntimeError(reply)
                if reply.startswith("OK "):
                    return reply[3:]
        raise TimeoutError(f"No response to {command!r} on {port}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="CDC device; auto-detected by default")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument(
        "--watch", type=float, metavar="SECONDS", help="Poll status at this interval"
    )
    parser.add_argument("command", nargs="*", default=["status"])
    args = parser.parse_args()
    if args.timeout <= 0 or (args.watch is not None and args.watch <= 0):
        parser.error("Timeout and watch interval must be positive")
    try:
        port = args.port or find_port()
        while True:
            reply = request(
                port, "status" if args.watch else " ".join(args.command), args.timeout
            )
            if reply.startswith("{"):
                print(
                    json.dumps(json.loads(reply), indent=None if args.watch else 2),
                    flush=True,
                )
            else:
                print(reply, flush=True)
            if not args.watch:
                return 0
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0
    except (OSError, serial.SerialException, RuntimeError, ValueError) as error:
        print(f"micctl: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
