#!/usr/bin/env python3
"""Acquire the TV's internal storage directly onto an existing USB filesystem."""

from __future__ import annotations

import argparse
import asyncio
import io
import os
from pathlib import Path, PurePosixPath
import secrets
import shlex
import subprocess
import tarfile
import time
import traceback

from archive_shell import configure_payloads
from dump_tv import Acquisition, MIB, REPOSITORY, digest_file, now, save_json
from samsung_tv_root.archive_root import probe
from samsung_tv_root.sdb import SdbClient, route_callback_host

SIZE_SUM_COMMAND = 'awk \'{n+=$1} END {printf "%.0f", n+0; print ""}\''


def size_command(files):
    # awk's default numeric output can switch to scientific notation above 2 GiB.
    return f"stat -c %s {files} 2>/dev/null | {SIZE_SUM_COMMAND}"


class UsbAcquisition(Acquisition):
    def __init__(
        self, output, host, callback, mount, directory, uncompressed_files=False
    ):
        super().__init__(output, host, callback, compression="tv")
        if (
            not directory.startswith("samsung-tv-dump-")
            or PurePosixPath(directory).name != directory
        ):
            raise ValueError("Use a single, dedicated samsung-tv-dump-* folder name")
        if (
            not mount.startswith("/opt/media/USBDrive")
            or ".." in PurePosixPath(mount).parts
        ):
            raise ValueError("Expected the inspected USB mount under /opt/media")
        self.mount, self.directory = mount, directory
        self.remote = "/proc/1/root" + mount + "/" + directory
        self.device = None
        self.uncompressed_files = uncompressed_files
        previous = self.manifest.get("usb_destination")
        if previous and previous != self.remote:
            raise ValueError("This manifest belongs to another USB destination")
        self.manifest["usb_destination"] = self.remote
        self.manifest["storage"] = (
            "TV USB; host artifacts require import before indexing"
        )
        self.manifest.setdefault("usb_token", secrets.token_hex(24))
        self.save()

    async def prepare(self, agent, minimum_free_gib):
        counter = await self.execute(
            agent, "{ echo 2147483648; echo 1; } | " + SIZE_SUM_COMMAND
        )
        if counter.strip() != "2147483649":
            raise RuntimeError("TV byte counter did not preserve a value above 2 GiB")
        print("TV byte counter verified above 2 GiB.", flush=True)
        mounts = await self.execute(agent, "cat /proc/1/mountinfo")
        records = [
            line for line in mounts.splitlines() if line.split()[4] == self.mount
        ]
        if len(records) != 1 or not any(
            f" - {fs} /dev/sda1 " in records[0] for fs in ("vfat", "exfat")
        ):
            raise RuntimeError(
                "The inspected USB partition is no longer mounted at the expected path"
            )
        source = shlex.quote("/proc/1/root" + self.mount)
        self.device = int((await self.execute(agent, f"stat -c %d {source}")).strip())
        space = await self.execute(agent, f"df -Pk {source}")
        available = int(space.splitlines()[-1].split()[3]) * 1024
        if available < minimum_free_gib * 1024**3:
            raise RuntimeError(
                f"USB has only {available / 1024**3:.2f} GiB free; need {minimum_free_gib:g} GiB"
            )
        root = shlex.quote(self.remote)
        token = shlex.quote(self.manifest["usb_token"])
        marker = await self.execute(
            agent,
            f"if test -e {root}; then cat {root}/.acquisition-token; else mkdir {root} && printf %s {token} >{root}/.acquisition-token && cat {root}/.acquisition-token; fi",
        )
        if marker.strip() not in (
            self.manifest["usb_token"],
            self.manifest["usb_token"] + "\\n",
        ):
            raise RuntimeError("Existing USB folder belongs to another acquisition")
        await self.execute(agent, f"printf %s {token} >{root}/.acquisition-token")
        self.manifest["usb_device_number"] = self.device
        self.manifest["usb_initial_free_bytes"] = available
        self.save()

    async def transfer(
        self, agent, name, producer, details, accepted=(0,), compress=False
    ):
        if compress:
            raise ValueError("USB acquisition compresses on the TV")
        original_name = name
        uncompressed = (
            self.uncompressed_files
            and details.get("kind") == "filesystem"
            and name not in self.manifest["artifacts"]
        )
        if uncompressed:
            name = name.removesuffix(".gz")
            producer = producer.removesuffix(" | gzip -1")
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Unsafe artifact name")
        destination = self.remote + "/" + name
        final = shlex.quote(destination)
        partial = shlex.quote(destination + ".partial")
        split_archive = details.get("kind") == "filesystem"
        prefix = destination + ".partial-part-"
        files = shlex.quote(prefix) + "*" if split_archive else partial
        count_bytes = size_command(files)
        previous = self.manifest["artifacts"].get(name)
        if previous:
            if previous.get("parts"):
                existing = shlex.join(
                    self.remote + "/" + part["name"] for part in previous["parts"]
                )
                check_command = "/bin/bash -c " + shlex.quote(
                    f"set -o pipefail; cat -- {existing} | sha256sum"
                )
            else:
                check_command = f"sha256sum {final}"
            check = await agent.execute(check_command, 300)
            if check.exit_code == 0 and check.stdout.split()[0] == previous["sha256"]:
                print("RESUME " + name, flush=True)
                return
        mount = shlex.quote("/proc/1/root" + self.mount)
        await self.execute(
            agent,
            f'test "$(stat -c %d {mount})" = {self.device} && mkdir -p {shlex.quote(str(PurePosixPath(destination).parent))}',
        )
        # A prior interrupted split may have more parts than the next attempt.
        # Clear only this artifact's temporary files before producing new data.
        await self.execute(agent, f"rm -f -- {files}")
        runtime = f"/run/samsung-tv-root/agent-{agent.identity.pid}/usb-{secrets.token_hex(8)}"
        d = shlex.quote(runtime)
        sink = (
            f"| split -b 1073741824 -d -a 4 - {shlex.quote(prefix)}"
            if split_archive
            else f">{partial}"
        )
        inner = (
            f'umask 077; set -o pipefail; d={d}; mkfifo "$d/hash-pipe" || exit 125; '
            'sha256sum <"$d/hash-pipe" >"$d/source-sha256" & hp=$!; '
            f'{{ {producer}; }} | tee "$d/hash-pipe" {sink}; rc=$?; '
            'wait "$hp" || rc=125; '
            f'cat -- {files} | sha256sum >"$d/stored-sha256" || rc=125; '
            f'{count_bytes} >"$d/size"; '
            f'sha256sum {files} >"$d/parts" || rc=125; '
            'printf "%s\\n" "$rc" >"$d/status"; rm -f "$d/hash-pipe"'
        )
        launch = f'mkdir -p {d} && chmod 700 {d} || exit 1; /usr/bin/setsid /bin/bash -c {shlex.quote(inner)} 3>&- </dev/null >{d}/log 2>&1 & p=$!; echo "$p" >{d}/pid'
        started, last = time.monotonic(), 0
        print("USB START " + name, flush=True)
        try:
            await self.execute(agent, launch)
            while True:
                await asyncio.sleep(2)
                status = await self.execute(
                    agent,
                    f'test "$(stat -c %d {mount})" = {self.device} || exit 1; if test -f {d}/status; then echo DONE; cat {d}/status {d}/source-sha256 {d}/stored-sha256 {d}/size; else echo RUNNING; {count_bytes}; fi',
                )
                lines = status.splitlines()
                if lines[0] == "DONE":
                    break
                if time.monotonic() - last >= 15:
                    print(
                        f"USB DATA {name}: {int(lines[1]) / MIB:.1f} MiB written",
                        flush=True,
                    )
                    last = time.monotonic()
            code, source_sha, stored_sha, size = (
                int(lines[1]),
                lines[2].split()[0],
                lines[3].split()[0],
                int(lines[4]),
            )
            log = await self.execute(agent, f"cat {d}/log")
            logpath = self.output / "logs" / (name.replace("/", "__") + ".log")
            logpath.write_text(log)
            if source_sha != stored_sha or code not in accepted:
                raise RuntimeError(
                    f"USB verification failed: source_exit={code}, sha256_match={source_sha == stored_sha}; see {logpath}"
                )
            part_metadata = []
            if split_archive:
                listing = await self.execute(agent, f"cat {d}/parts")
                for line in listing.splitlines():
                    digest, old_path = line.split(None, 1)
                    old_path = old_path.strip()
                    if (
                        not old_path.startswith(prefix)
                        or not old_path[len(prefix) :].isdigit()
                    ):
                        raise RuntimeError("Unexpected split archive filename")
                    part_name = name + ".part-" + old_path[len(prefix) :]
                    part_size = int(
                        await self.execute(agent, f"stat -c %s {shlex.quote(old_path)}")
                    )
                    await self.execute(
                        agent,
                        f"mv -- {shlex.quote(old_path)} {shlex.quote(self.remote + '/' + part_name)}",
                    )
                    part_metadata.append(
                        {"name": part_name, "bytes": part_size, "sha256": digest}
                    )
                if sum(part["bytes"] for part in part_metadata) != size:
                    raise RuntimeError("Split archive sizes do not match")
            else:
                await self.execute(agent, f"mv -- {partial} {final}")
            self.manifest["artifacts"][name] = {
                **details,
                "bytes": size,
                "sha256": stored_sha,
                "source_sha256": source_sha,
                "source_exit": code,
                "warnings": bool(log.strip()),
                "log": str(logpath.relative_to(self.output)),
                "completed": now(),
                "seconds": round(time.monotonic() - started, 2),
                "compression_location": "none" if uncompressed else "tv",
                "archive_compression": "none" if uncompressed else "gzip",
                "verification": "producer SHA-256 equals USB readback SHA-256",
                **({"parts": part_metadata} if part_metadata else {}),
            }
            if details.get("kind") == "filesystem":
                for error in self.manifest["errors"]:
                    if error.get("phase") == "filesystems" and error.get(
                        "source"
                    ) == details.get("mount"):
                        error["resolved_by"] = name
                        error["resolved"] = now()
            if uncompressed:
                await self.execute(
                    agent,
                    "rm -f -- "
                    + shlex.quote(self.remote + "/" + original_name + ".partial-part-")
                    + "*",
                )
            self.save()
            print(
                f"USB DONE {name}: {size / MIB:.1f} MiB, readback SHA-256 verified, exit={code}",
                flush=True,
            )
        finally:
            cleanup = f'd={d}; if test -r "$d/pid"; then p=$(cat "$d/pid"); case "$p" in ""|*[!0-9]*) ;; *) c=$(tr "\\0" " " </proc/$p/cmdline 2>/dev/null); case "$c" in *"$d"*) kill -TERM -- "-$p" 2>/dev/null;; esac;; esac; fi; rm -rf -- "$d"'
            await self.execute(agent, cleanup, 20)

    async def publish_metadata(self, agent):
        sums = ""
        for name, details in self.manifest["artifacts"].items():
            if details.get("parts"):
                sums += "".join(
                    f"{part['sha256']}  {part['name']}\n" for part in details["parts"]
                )
            else:
                sums += f"{details['sha256']}  {name}\n"
        (self.output / "SHA256SUMS").write_text(sums)
        (self.output / "index_dump.py").write_bytes(
            (REPOSITORY / "tools/index_dump.py").read_bytes()
        )
        (self.output / "README.md").write_text(
            "# Samsung TV USB dump\n\nFilesystem exports (gzip or plain tar, split into parts) and raw storage chunks are in `archives/` and `images/`. Each artifact was hashed during creation and read back from USB to verify it. See `manifest.json` for coverage, timestamps and any errors.\n\nCopy this entire directory to the Mac's repository at `dist/dump`, then run:\n\n```sh\npython3 tools/index_dump.py\n```\n\nAlternatively, from a copied dump directory run `python3 index_dump.py --output .`. This builds `rootfs/`, `index.sqlite`, assembled images and a study guide. Allow additional disk space for extraction and assembled images. The dump is live, not an atomic snapshot. Virtual filesystems, RAM and RPMB are excluded. The TV's tar lacks xattr support; raw images preserve the underlying bytes.\n"
        )
        read_errors = [
            path
            for artifact in self.manifest["artifacts"].values()
            for path in artifact.get("read_errors", [])
        ]
        if read_errors:
            with (self.output / "README.md").open("a") as guide:
                guide.write(
                    "\nProtected paths omitted from file exports: "
                    + ", ".join(f"`{path}`" for path in read_errors)
                    + ". Their underlying storage is retained in the raw device images.\n"
                )
        records = []
        for path in sorted(self.output.rglob("*")):
            if not path.is_file() or path.suffix == ".partial":
                continue
            relative = path.relative_to(self.output).as_posix()
            records.append((relative, path.read_bytes()))
        # One small staging archive avoids hundreds of round trips over Wi-Fi.
        bundle = io.BytesIO()
        with tarfile.open(fileobj=bundle, mode="w") as archive:
            for relative, data in records:
                entry = tarfile.TarInfo(relative)
                entry.size, entry.mode = len(data), 0o600
                entry.mtime = int(time.time())
                archive.addfile(entry, io.BytesIO(data))
        if len(bundle.getvalue()) <= 4 * MIB:
            staged = f"/run/samsung-tv-root/agent-{agent.identity.pid}/usb-metadata-{secrets.token_hex(8)}"
            await agent.write_file(staged, bundle.getvalue(), timeout=60)
            await self.execute(
                agent,
                f"tar --no-same-owner --no-same-permissions -C {shlex.quote(self.remote)} -xf {shlex.quote(staged)} && rm -f -- {shlex.quote(staged)}",
                300,
            )
        else:
            for relative, data in records:
                staged = f"/run/samsung-tv-root/agent-{agent.identity.pid}/usb-metadata-{secrets.token_hex(8)}"
                await agent.write_file(staged, data, timeout=60)
                remote = self.remote + "/" + relative
                await self.execute(
                    agent,
                    f"mkdir -p {shlex.quote(str(PurePosixPath(remote).parent))} && cp -- {shlex.quote(staged)} {shlex.quote(remote)} && rm -f -- {shlex.quote(staged)}",
                )
        await self.execute(agent, "sync", 300)
        for name in ("manifest.json", "SHA256SUMS", "index_dump.py", "README.md"):
            observed = await self.execute(
                agent, "sha256sum " + shlex.quote(self.remote + "/" + name), 60
            )
            if observed.split()[0] != digest_file(self.output / name):
                raise RuntimeError("USB metadata verification failed: " + name)
        print("USB metadata saved; writes synchronized.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default=os.environ.get("TV_HOST"),
        help="TV IPv4 address (or set TV_HOST)",
    )
    parser.add_argument("--callback-host")
    parser.add_argument("--mount", default="/opt/media/USBDriveA1")
    parser.add_argument("--directory", required=True)
    parser.add_argument(
        "--output", type=Path, default=REPOSITORY / "dist/dump/usb-acquisition"
    )
    parser.add_argument("--phase", choices=("all", "files", "images"), default="all")
    parser.add_argument("--minimum-free-gib", type=float, default=15)
    parser.add_argument(
        "--uncompressed-files",
        action="store_true",
        help="Capture missing filesystem exports as uncompressed tar parts",
    )
    args = parser.parse_args()
    if not args.host:
        parser.error("provide --host or set TV_HOST")
    os.umask(0o077)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    configure_payloads()
    adapter = REPOSITORY / "tools/sdb_docker.py"
    os.environ["SDB"] = str(adapter)
    callback = args.callback_host or route_callback_host(args.host)
    acquisition = UsbAcquisition(
        output, args.host, callback, args.mount, args.directory, args.uncompressed_files
    )
    subprocess.run(
        ["docker", "start", os.environ.get("SAMSUNG_TV_SDB_CONTAINER", "tizen-sdb")],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    async def session(agent):
        try:
            await acquisition.prepare(agent, args.minimum_free_gib)
            result = await acquisition.session(agent, args.phase)
            await acquisition.publish_metadata(agent)
            return result
        except BaseException:
            (output / "metadata/session-error.txt").write_text(traceback.format_exc())
            raise

    try:
        evidence = asyncio.run(
            probe(args.host, callback_host=callback, session_handler=session)
        )
        save_json(output / "metadata/root-session.json", evidence)
    finally:
        SdbClient(adapter, args.host).disconnect()


if __name__ == "__main__":
    main()
