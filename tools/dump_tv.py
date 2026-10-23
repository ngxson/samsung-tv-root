#!/usr/bin/env python3
"""Resume a read-only TV acquisition into local archives and raw image chunks."""

from __future__ import annotations

import argparse
import asyncio
from datetime import datetime, timezone
import hashlib
import gzip
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import socket
import subprocess
import time

from archive_shell import configure_payloads
from samsung_tv_root.archive_root import probe
from samsung_tv_root.sdb import SdbClient, route_callback_host

REPOSITORY = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024


def now():
    return datetime.now(timezone.utc).isoformat()


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def digest_file(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            h.update(block)
    return h.hexdigest()


class Acquisition:
    def __init__(self, output, host, callback, compression="tv"):
        self.output, self.host, self.callback = output, host, callback
        self.compression = compression
        self.manifest_path = output / "manifest.json"
        self.manifest = (
            json.loads(self.manifest_path.read_text())
            if self.manifest_path.exists()
            else {
                "started": now(),
                "host": host,
                "method": "archive-root; original /proc/1/root view",
                "artifacts": {},
                "errors": [],
                "limitations": [
                    "Live acquisition: writable filesystems are not an atomic snapshot.",
                    "Virtual filesystems and RAM are not archived as files.",
                    "RPMB is not captured: it is not an ordinary block device.",
                ],
            }
        )
        if self.manifest["host"] != host:
            raise ValueError("Output belongs to another TV host")
        for name in ("metadata", "archives", "images", "logs"):
            (output / name).mkdir(parents=True, exist_ok=True)

    def save(self):
        self.manifest["updated"] = now()
        save_json(self.manifest_path, self.manifest)

    async def execute(self, agent, command, timeout=60):
        r = await agent.execute(command, timeout)
        if r.timed_out or r.exit_code != 0:
            raise RuntimeError(
                f"Command failed ({r.exit_code}, timeout={r.timed_out}): {r.stderr[:1000]}"
            )
        return r.stdout

    async def inventory(self, agent):
        commands = {
            "global-mountinfo": "cat /proc/1/mountinfo",
            "partitions-current": "cat /proc/partitions; ls -l /dev/disk/by-partlabel",
            "block-sizes": 'for p in /sys/class/block/mmcblk0 /sys/class/block/mmcblk0boot* /sys/class/block/subroot*; do [ -r "$p/size" ] || continue; printf "%s " "${p##*/}"; cat "$p/size"; done',
            "block-layout": 'for p in /sys/class/block/mmcblk0p* /sys/class/block/subroot*; do test -r "$p/start" || continue; printf "%s " "${p##*/}"; cat "$p/start" "$p/size" | tr "\\n" " "; echo; done',
            "processes": "ps -ef",
            "services": "systemctl list-units --all --no-pager --no-legend",
            "labels-tools": "command -v getfattr getcap; ls -ldZ / /usr /opt /proc/1/root; cat /proc/version",
            "block-read-check": 'for p in /dev/mmcblk0 /dev/mmcblk0boot0 /dev/mmcblk0boot1 /dev/subroot1 /dev/subroot2; do echo "$p"; dd if="$p" of=/dev/null bs=512 count=1; done',
            "network-link": 'for p in /sys/class/net/*; do echo "${p##*/}"; cat "$p/speed" "$p/operstate" 2>/dev/null; done; cat /proc/net/wireless',
            "process-limits": "cat /proc/self/limits",
            "compression-diagnostics": "dmesg | grep -Ei 'gzip|18826|19317|oom' | tail -60",
            "file-capabilities-system": "getcap -r /proc/1/root/usr",
            "file-capabilities-apps": "getcap -r /proc/1/root/opt/usr/apps",
        }
        for name, command in commands.items():
            r = await agent.execute(command, 60)
            (self.output / "metadata" / (name + ".txt")).write_text(r.stdout)
            save_json(
                self.output / "metadata" / (name + ".status.json"),
                {
                    "command": command,
                    "exit_code": r.exit_code,
                    "timed_out": r.timed_out,
                    "stderr": r.stderr,
                },
            )
        self.manifest["inventory_time"] = now()
        self.save()

    async def transfer(
        self, agent, name, producer, details, accepted=(0,), compress=False
    ):
        destination = self.output / name
        previous = self.manifest["artifacts"].get(name)
        if (
            previous
            and destination.is_file()
            and destination.stat().st_size == previous["bytes"]
        ):
            if digest_file(destination) == previous["sha256"]:
                print(f"RESUME {name}: verified, skipping", flush=True)
                return
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(destination.suffix + ".partial")
        token = secrets.token_hex(24)
        remote = f"/run/samsung-tv-root/agent-{agent.identity.pid}/dump-{secrets.token_hex(8)}"
        d = shlex.quote(remote)
        loop = asyncio.get_running_loop()
        received = loop.create_future()
        writers = []
        started = time.monotonic()

        async def receive(reader, writer):
            if writer.get_extra_info("peername")[0] != self.host:
                writer.close()
                return
            try:
                header = await asyncio.wait_for(reader.readline(), 10)
                if header != f"TV-DUMP/1 {token}\n".encode() or writers:
                    writer.close()
                    return
                writers.append(writer)
                h, count, last = hashlib.sha256(), 0, time.monotonic()
                with temporary.open("wb") as stream:
                    target = (
                        gzip.GzipFile(
                            fileobj=stream, mode="wb", compresslevel=1, mtime=0
                        )
                        if compress
                        else stream
                    )
                    while True:
                        block = await asyncio.wait_for(reader.read(MIB), 90)
                        if not block:
                            break
                        target.write(block)
                        h.update(block)
                        count += len(block)
                        if time.monotonic() - last >= 15:
                            print(
                                f"DATA {name}: {count / MIB:.1f} MiB received",
                                flush=True,
                            )
                            last = time.monotonic()
                    if compress:
                        target.close()
                    stream.flush()
                    os.fsync(stream.fileno())
                if not received.done():
                    received.set_result((count, h.hexdigest()))
            except Exception as error:
                if not received.done():
                    received.set_exception(error)
            finally:
                writer.close()
                await writer.wait_closed()

        server = await asyncio.start_server(receive, self.callback, 0, limit=2 * MIB)
        for listener in server.sockets:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * MIB)
        port = server.sockets[0].getsockname()[1]
        socat = "/opt/usr/apps/com.samsung.tv.iot-hub-control/bin/socat"
        inner = (
            f"umask 077; set -o pipefail; d={d}; "
            'mkfifo "$d/hash-pipe" || exit 125; '
            'sha256sum <"$d/hash-pipe" >"$d/sha256" & hp=$!; '
            f'{{ printf "%s\\n" "TV-DUMP/1 {token}"; '
            f'{{ {producer}; }} | tee "$d/hash-pipe"; }} | '
            f"{socat} -u -b 1048576 STDIN TCP:{self.callback}:{port},nodelay,sndbuf=1048576; rc=$?; "
            'wait "$hp" || rc=125; printf "%s\\n" "$rc" >"$d/status"; '
            'rm -f "$d/hash-pipe"'
        )
        launch = (
            f"mkdir -p {d} && chmod 700 {d} || exit 1; "
            f"/usr/bin/setsid /bin/bash -c {shlex.quote(inner)} "
            f'</dev/null >{d}/log 2>&1 & p=$!; printf "%s\\n" "$p" >{d}/pid'
        )
        print(f"START {name}", flush=True)
        try:
            await self.execute(agent, launch)
            while not received.done():
                try:
                    await asyncio.wait_for(asyncio.shield(received), 30)
                except asyncio.TimeoutError:
                    if not writers:
                        result = await agent.execute(
                            f"cat {d}/log; test -f {d}/status && cat {d}/status", 15
                        )
                        raise RuntimeError(
                            "No authenticated data callback: " + result.stdout[:1000]
                        ) from None
                    diagnostic = await agent.execute(
                        "ps -eo pid,ppid,state,pcpu,comm | grep -E 'tar|gzip|socat|sha256|tee'; cat /proc/loadavg",
                        10,
                    )
                    (self.output / "logs" / "transfer-load.txt").write_text(
                        diagnostic.stdout
                    )
            count, digest = received.result()
            info = await self.execute(
                agent,
                f"for i in 1 2 3 4 5; do test -f {d}/status && break; sleep 1; done; cat {d}/status; cat {d}/sha256; cat {d}/log",
            )
            lines = info.splitlines()
            code = int(lines[0])
            expected = lines[1].split()[0]
            log = "\n".join(lines[2:])
            logpath = self.output / "logs" / (name.replace("/", "__") + ".log")
            logpath.write_text(log + "\n")
            if digest != expected or code not in accepted:
                raise RuntimeError(
                    f"Transfer verification failed: source_exit={code}, sha256_match={digest == expected}; see {logpath}"
                )
            temporary.replace(destination)
            stored_digest = digest_file(destination) if compress else digest
            stored_bytes = destination.stat().st_size
            self.manifest["artifacts"][name] = {
                **details,
                "bytes": stored_bytes,
                "sha256": stored_digest,
                "source_sha256": expected,
                "stream_sha256": digest,
                "stream_bytes": count,
                "compression_location": "host"
                if compress
                else "tv"
                if details.get("kind") in ("filesystem", "block-device")
                else "none",
                "source_exit": code,
                "warnings": bool(log.strip()),
                "log": str(logpath.relative_to(self.output)),
                "completed": now(),
                "seconds": round(time.monotonic() - started, 2),
            }
            self.save()
            print(
                f"DONE {name}: {stored_bytes / MIB:.1f} MiB stored, source SHA-256 verified, exit={code}",
                flush=True,
            )
        finally:
            server.close()
            await server.wait_closed()
            for writer in writers:
                writer.close()
            cleanup = (
                f'd={d}; if [ -r "$d/pid" ]; then p=$(cat "$d/pid"); '
                'case "$p" in ""|*[!0-9]*) ;; *) '
                'c=$(tr "\\0" " " </proc/$p/cmdline 2>/dev/null); '
                'case "$c" in *"$d"*) kill -TERM -- "-$p" 2>/dev/null;; esac;; esac; fi; '
                'rm -rf -- "$d"'
            )
            await self.execute(agent, cleanup, 20)

    async def filesystems(self, agent):
        mounts = []
        text = (self.output / "metadata/global-mountinfo.txt").read_text()
        for line in text.splitlines():
            left, right = line.split(" - ", 1)
            fields, extra = left.split(), right.split()
            if (
                re.fullmatch(r"/dev/(root|subroot\d+|mmcblk0p\d+|loop\d+)", extra[1])
                and fields[3] == "/"
            ):
                mount = fields[4]
                if "\\" in mount:
                    raise RuntimeError(
                        "Escaped mount name needs explicit review: " + mount
                    )
                mounts.append(
                    {"mount": mount, "device": extra[1], "filesystem": extra[0]}
                )
        self.manifest["filesystems"] = mounts
        self.save()
        for entry in sorted(mounts, key=lambda entry: entry["mount"] in ("/", "/opt")):
            mount = entry["mount"]
            name = "root" if mount == "/" else mount.strip("/").replace("/", "__")
            source = "/proc/1/root" + (mount if mount != "/" else "")
            exclusions = (
                [
                    "--exclude=./usr/home/owner/share/tmp/sdk_tools/archive-root-*",
                    "--exclude=./usr/home/owner/share/tmp/sdk_tools/on-demand/archive-root-*",
                ]
                if mount == "/opt"
                else []
            )
            command = shlex.join(
                [
                    "tar",
                    "--format=posix",
                    "--numeric-owner",
                    "--xattrs",
                    "--acls",
                    "--sparse",
                    "--one-file-system",
                    *exclusions,
                    "-C",
                    source,
                    "-cf",
                    "-",
                    ".",
                ]
            )
            try:
                await self.transfer(
                    agent,
                    f"archives/{name}.tar.gz",
                    command + (" | gzip -1" if self.compression == "tv" else ""),
                    {"kind": "filesystem", **entry, "source_view": source},
                    accepted=(0, 1),
                    compress=self.compression == "host",
                )
            except Exception as error:
                self.manifest["errors"].append(
                    {
                        "phase": "filesystems",
                        "source": mount,
                        "error": str(error),
                        "time": now(),
                    }
                )
                self.save()
                print(f"FAILED filesystem {mount}: {error}", flush=True)

    async def images(self, agent):
        text = (self.output / "metadata/block-sizes.txt").read_text()
        for line in text.splitlines():
            name, sectors = line.split()
            size = int(sectors) * 512
            if not size:
                continue
            for offset in range(0, size, 128 * MIB):
                length = min(128 * MIB, size - offset)
                if offset % MIB or length % 512:
                    raise RuntimeError("Unexpected block alignment")
                command = shlex.join(
                    [
                        "dd",
                        f"if=/dev/{name}",
                        "bs=1048576",
                        f"skip={offset // MIB}",
                        f"count={(length + MIB - 1) // MIB}",
                    ]
                )
                filename = f"images/{name}/{offset:012x}.img.gz"
                try:
                    await self.transfer(
                        agent,
                        filename,
                        command + (" | gzip -1" if self.compression == "tv" else ""),
                        {
                            "kind": "block-device",
                            "device": "/dev/" + name,
                            "device_bytes": size,
                            "offset": offset,
                            "uncompressed_bytes": length,
                        },
                        compress=self.compression == "host",
                    )
                except Exception as error:
                    self.manifest["errors"].append(
                        {
                            "phase": "images",
                            "source": name,
                            "offset": offset,
                            "error": str(error),
                            "time": now(),
                        }
                    )
                    self.save()
                    print(f"FAILED block device {name}: {error}", flush=True)
                    break

    async def session(self, agent, phase):
        self.manifest["active_phase"] = phase
        self.manifest["session_started"] = now()
        self.manifest.pop("session_finished", None)
        interrupted = self.output / "metadata/interrupted-acquisitions.json"
        if interrupted.exists():
            packages = json.loads(interrupted.read_text())["packages"]
            paths = []
            for package in packages:
                if not re.fullmatch(r"archive-root-[a-f0-9]{16}", package):
                    raise ValueError("Invalid interrupted acquisition package")
                staging = "/home/owner/share/tmp/sdk_tools/" + package
                archive = (
                    "/home/owner/share/tmp/sdk_tools/on-demand/" + package + ".tar.gz"
                )
                paths.extend([staging, archive, archive + ".signature"])
            if paths:
                quoted = shlex.join(paths)
                await self.execute(
                    agent,
                    f'rm -rf -- {quoted}; for p in {quoted}; do test ! -e "$p" || exit 1; done',
                )
                save_json(
                    self.output / "metadata/interrupted-cleanup.json",
                    {"packages": packages, "removed": True, "time": now()},
                )
        await self.inventory(agent)
        if phase in ("all", "files"):
            await self.filesystems(agent)
        if phase in ("all", "images"):
            await self.images(agent)
        self.manifest["session_finished"] = now()
        self.save()
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default=os.environ.get("TV_HOST"),
        help="TV IPv4 address (or set TV_HOST)",
    )
    parser.add_argument("--callback-host")
    parser.add_argument("--output", type=Path, default=REPOSITORY / "dist/dump")
    parser.add_argument(
        "--compression",
        choices=("tv", "host"),
        default="tv",
        help="Compress on the TV to save network bandwidth, or on the host to reduce TV CPU use",
    )
    parser.add_argument(
        "--phase", choices=("all", "files", "images", "inventory"), default="all"
    )
    arguments = parser.parse_args()
    if not arguments.host:
        parser.error("provide --host or set TV_HOST")
    os.umask(0o077)
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    configure_payloads()
    adapter = REPOSITORY / "tools/sdb_docker.py"
    os.environ["SDB"] = str(adapter)
    callback = arguments.callback_host or route_callback_host(arguments.host)
    acquisition = Acquisition(output, arguments.host, callback, arguments.compression)
    subprocess.run(
        ["docker", "start", os.environ.get("SAMSUNG_TV_SDB_CONTAINER", "tizen-sdb")],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    try:
        evidence = asyncio.run(
            probe(
                arguments.host,
                callback_host=callback,
                session_handler=lambda agent: acquisition.session(
                    agent, arguments.phase
                ),
            )
        )
        save_json(
            output / "metadata" / f"root-session-{int(time.time())}.json", evidence
        )
    finally:
        SdbClient(adapter, arguments.host).disconnect()


if __name__ == "__main__":
    main()
