#!/usr/bin/env python3
"""Verify a TV dump, extract regular files safely, and build its study index."""

from __future__ import annotations

import argparse
from collections import defaultdict
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sqlite3
import tarfile
import tempfile
import unicodedata

MIB = 1024 * 1024


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def archive_path(mount, name):
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"Unsafe archive member: {name!r}")
    return str(PurePosixPath(mount) / relative)


class Study:
    def __init__(self, output):
        self.output = output.resolve()
        self.manifest = json.loads((output / "manifest.json").read_text())
        self.root = self.output / "rootfs"
        self.root.mkdir(exist_ok=True)
        self.names = {}
        self.collisions = []
        self.db_path = self.output / "index.sqlite.partial"
        self.db_path.unlink(missing_ok=True)
        self.db = sqlite3.connect(self.db_path)
        self.db.execute("""CREATE TABLE files (
            path TEXT, kind TEXT, size INTEGER, mode INTEGER, uid INTEGER,
            gid INTEGER, mtime REAL, link_target TEXT, sha256 TEXT,
            format TEXT, archive TEXT, local_path TEXT, pax_headers TEXT,
            PRIMARY KEY (path, archive))""")
        self.db.execute("CREATE INDEX files_path ON files(path)")
        self.db.execute("CREATE INDEX files_format ON files(format)")
        self.verified = []
        self.images = []
        self.metadata_warnings = []

    def verify(self, name, details):
        path = self.output / name
        if details.get("parts"):
            temporary = path.with_suffix(path.suffix + ".joining")
            with temporary.open("wb") as target:
                for part in details["parts"]:
                    piece = self.output / part["name"]
                    if (
                        piece.stat().st_size != part["bytes"]
                        or digest_file(piece) != part["sha256"]
                    ):
                        raise ValueError(
                            "Split archive part verification failed: " + part["name"]
                        )
                    with piece.open("rb") as source:
                        for block in iter(lambda: source.read(MIB), b""):
                            target.write(block)
                    self.verified.append((part["sha256"], part["name"]))
            if (
                temporary.stat().st_size != details["bytes"]
                or digest_file(temporary) != details["sha256"]
            ):
                raise ValueError("Reassembled archive verification failed: " + name)
            temporary.replace(path)
        if path.stat().st_size != details["bytes"]:
            raise ValueError("Size mismatch: " + name)
        if digest_file(path) != details["sha256"]:
            raise ValueError("SHA-256 mismatch: " + name)
        self.verified.append((details["sha256"], name))
        return path

    def destination(self, path):
        parts = PurePosixPath(path).parts[1:]
        collision = False
        for count in range(1, len(parts) + 1):
            prefix = "/".join(parts[:count])
            key = unicodedata.normalize("NFC", prefix).casefold()
            previous = self.names.setdefault(key, prefix)
            if previous != prefix:
                collision = True
        if collision:
            destination = (
                self.output / "collisions" / hashlib.sha256(path.encode()).hexdigest()
            )
            self.collisions.append(
                {"path": path, "local_path": str(destination.relative_to(self.output))}
            )
        else:
            destination = self.root.joinpath(*parts)
        # Never follow a link left by a previous extraction or a local edit.
        for component in (destination, *destination.parents):
            if component == self.output:
                break
            if component.is_symlink():
                raise ValueError("Refusing local symlink: " + str(component))
        destination.parent.mkdir(parents=True, exist_ok=True)
        return destination

    def filesystem(self, name, details):
        print("INDEX " + name, flush=True)
        archive = self.verify(name, details)
        log = self.output / details.get("log", "logs/missing.log")
        if (
            log.is_file()
            and "Xattr support requested, but not available" in log.read_text()
        ):
            warning = "The TV's tar lacks xattr support. File exports do not preserve extended attributes/SMACK labels; raw images preserve the underlying storage bytes."
            if warning not in self.metadata_warnings:
                self.metadata_warnings.append(warning)
        opener = gzip.open if name.endswith(".gz") else open
        with opener(archive, "rb") as compressed:
            with tarfile.open(fileobj=compressed, mode="r|") as tar:
                for member in tar:
                    path = archive_path(details["mount"], member.name)
                    sha, local, file_format = None, None, None
                    kind = (
                        "file"
                        if member.isfile()
                        else "directory"
                        if member.isdir()
                        else "symlink"
                        if member.issym()
                        else "hardlink"
                        if member.islnk()
                        else "special"
                    )
                    if member.isfile():
                        destination = self.destination(path)
                        h = hashlib.sha256()
                        source = tar.extractfile(member)
                        head = source.read(64)
                        file_format = (
                            "ELF"
                            if head.startswith(b"\x7fELF")
                            else "PE/MZ"
                            if head.startswith(b"MZ")
                            else "script"
                            if head.startswith(b"#!")
                            else None
                        )
                        with tempfile.NamedTemporaryFile(
                            dir=destination.parent, prefix=".extract-", delete=False
                        ) as target:
                            temporary = Path(target.name)
                            target.write(head)
                            h.update(head)
                            for block in iter(
                                lambda source=source: source.read(MIB), b""
                            ):
                                target.write(block)
                                h.update(block)
                        if temporary.stat().st_size != member.size:
                            raise ValueError("Truncated member: " + path)
                        temporary.replace(destination)
                        sha = h.hexdigest()
                        local = str(destination.relative_to(self.output))
                    # Links, permissions, ownership, xattrs and special nodes are
                    # indexed, not applied to the host filesystem.
                    self.db.execute(
                        "INSERT OR REPLACE INTO files VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (
                            path,
                            kind,
                            member.size,
                            member.mode,
                            member.uid,
                            member.gid,
                            member.mtime,
                            member.linkname or None,
                            sha,
                            file_format,
                            name,
                            local,
                            json.dumps(member.pax_headers, ensure_ascii=True),
                        ),
                    )
            # tar ends before gzip's trailer: consume it to verify its CRC too.
            while compressed.read(MIB):
                pass
        self.db.commit()

    def block_images(self):
        devices = defaultdict(list)
        for name, details in self.manifest["artifacts"].items():
            if details["kind"] == "block-device":
                devices[details["device"]].append((name, details))
        for device, chunks in devices.items():
            chunks.sort(key=lambda item: item[1]["offset"])
            position = 0
            for _, details in chunks:
                if details["offset"] != position:
                    break
                position += details["uncompressed_bytes"]
            if position != chunks[0][1]["device_bytes"]:
                print("INCOMPLETE device, keeping chunks only: " + device, flush=True)
                continue
            destination = self.output / "images" / (PurePosixPath(device).name + ".img")
            temporary = destination.with_suffix(".img.partial")
            h = hashlib.sha256()
            print("ASSEMBLE " + device, flush=True)
            with temporary.open("wb") as target:
                for name, details in chunks:
                    source = self.verify(name, details)
                    length = 0
                    with gzip.open(source, "rb") as stream:
                        for block in iter(lambda: stream.read(MIB), b""):
                            target.write(block)
                            h.update(block)
                            length += len(block)
                    if length != details["uncompressed_bytes"]:
                        raise ValueError("Unexpected decompressed size: " + name)
            temporary.replace(destination)
            name = str(destination.relative_to(self.output))
            self.verified.append((h.hexdigest(), name))
            self.images.append(
                {
                    "device": device,
                    "path": name,
                    "bytes": position,
                    "sha256": h.hexdigest(),
                }
            )

    def partition_map(self):
        labels = {}
        source = self.output / "metadata/partitions-current.txt"
        if source.exists():
            for line in source.read_text().splitlines():
                match = re.search(r" (\S+) -> ../../(mmcblk0p\d+)$", line)
                if match:
                    labels[match[2]] = match[1]
        rows = []
        for line in (
            (self.output / "metadata/block-layout.txt").read_text().splitlines()
        ):
            device, start, sectors = line.split()
            rows.append(
                {
                    "device": device,
                    "label": labels.get(device, ""),
                    "offset": int(start) * 512,
                    "bytes": int(sectors) * 512,
                }
            )
        rows.sort(key=lambda row: row["offset"])
        (self.output / "partitions.json").write_text(json.dumps(rows, indent=2) + "\n")

    def finish(self):
        counts = dict(self.db.execute("SELECT kind, count(*) FROM files GROUP BY kind"))
        formats = dict(
            self.db.execute(
                "SELECT format, count(*) FROM files WHERE format IS NOT NULL GROUP BY format"
            )
        )
        self.db.commit()
        self.db.close()
        self.db_path.replace(self.output / "index.sqlite")
        (self.output / "collisions.json").write_text(
            json.dumps(self.collisions, indent=2) + "\n"
        )
        (self.output / "study.json").write_text(
            json.dumps(
                {
                    "counts": counts,
                    "formats": formats,
                    "images": self.images,
                    "case_collisions": len(self.collisions),
                },
                indent=2,
            )
            + "\n"
        )
        (self.output / "SHA256SUMS").write_text(
            "".join(
                f"{sha}  {name}\n"
                for sha, name in sorted(self.verified, key=lambda item: item[1])
            )
        )
        artifact_rows = "\n".join(
            f"| `{name}` | {details['bytes'] / MIB:.1f} MiB | {details['source_exit']} |"
            for name, details in self.manifest["artifacts"].items()
            if details["kind"] == "filesystem"
        )
        limitations = "\n".join(
            "- " + text
            for text in self.manifest["limitations"] + self.metadata_warnings
        )
        errors = self.manifest["errors"]
        unresolved = [error for error in errors if not error.get("resolved_by")]
        read_errors = [
            path
            for artifact in self.manifest["artifacts"].values()
            for path in artifact.get("read_errors", [])
        ]
        guide = f"""# Samsung TV system dump

Captured from a TQ43Q7F3AUXXC (25_RSL_QTV), Tizen 9.0.0 / Linux 5.4.261 ARMv7.
Firmware build: `T-RSLFDEUC-0090-REL-202608061939`.
Acquisition began {self.manifest['started']}; see `manifest.json` for per-artifact times.

## Start studying

- `rootfs/`: extracted regular files in their TV paths; contents are unchanged.
- `index.sqlite`: {sum(counts.values()):,} archive entries; {formats.get('ELF', 0):,} ELF files and {formats.get('PE/MZ', 0):,} PE/MZ files.
- `archives/`: original filesystem exports with numeric ownership, modes, link targets and available ACL/xattr metadata.
- `images/*.img`: assembled raw devices; compressed chunks remain in per-device directories.
- `partitions.json`: partition offsets and lengths in bytes, with the TV's labels.
- `metadata/`: platform, kernel, mounts, processes, services and storage inventory.
- `manifest.json`, `SHA256SUMS`, `logs/`: acquisition provenance, verification and read warnings.

```sh
# Run from dist/dump
rg -n 'ExecStart=' rootfs/usr/lib/systemd rootfs/etc/systemd
sqlite3 -header -column index.sqlite \\
  \"SELECT path, size FROM files WHERE format='ELF' ORDER BY size DESC LIMIT 30;\"
sqlite3 -header -column index.sqlite \\
  \"SELECT path, link_target FROM files WHERE kind IN ('symlink','hardlink');\"
sqlite3 -header -column index.sqlite \\
  \"SELECT path, mode, uid, gid, pax_headers FROM files WHERE path LIKE '%sdb%';\"
shasum -a 256 -c SHA256SUMS
```

Start with `rootfs/etc`, `rootfs/usr/bin`, `rootfs/usr/sbin`, `rootfs/usr/lib`,
`rootfs/usr/share/dotnet` and `rootfs/opt/usr/apps`. Links are recorded in SQLite;
they are not created on the Mac. Device nodes, sockets and FIFOs are not created.
Host files use private permissions; original Linux metadata remains in the archives
and index. Case-insensitive path collisions are stored in `collisions/`, with
their original paths in `collisions.json` and SQLite's `local_path` column.
The acquired executables have not been run on the Mac.

## Filesystem exports

| Archive | Stored size | Source exit |
| --- | ---: | ---: |
{artifact_rows}

Each completed transfer matched SHA-256 computed on the TV and Mac. The indexer
checked local hashes, gzip checksums, archive file lengths and raw chunk lengths.
Exit 1 from GNU tar means files changed while being read; consult the archive log.
Exports with exit 2 have documented file-level read errors. These paths were not
exported as files: {', '.join(f'`{path}`' for path in read_errors) or 'none'}.
Their underlying storage is retained in the raw device images.

## Scope and limitations

{limitations}
- Files were read through `/proc/1/root` to avoid the root session's temporary `/etc/passwd` and verification-key overlays.
- This acquisition's temporary SDK staging files were excluded from file exports.
- Live raw images and file exports were captured at different times; they are analysis material, not a tested restoration backup.
- Acquisition errors still unresolved: {len(unresolved)} ({len(errors)} total attempts recorded). See `manifest.json`; incomplete `.partial` files are not verified artifacts.
- The dump includes private TV state. Keep it local; review selected files before sharing.

To resume acquisition from the repository root:

```sh
# Export TV_HOST with your TV's address before running this command.
PYTHONPATH=src:tools python3 tools/dump_tv.py --phase all
python3 tools/index_dump.py
```
"""
        (self.output / "README.md").write_text(guide)
        print(
            f"READY: {sum(counts.values()):,} entries; {len(self.images)} assembled devices",
            flush=True,
        )

    def run(self):
        for name, details in self.manifest["artifacts"].items():
            if details["kind"] == "filesystem":
                self.filesystem(name, details)
        self.block_images()
        self.partition_map()
        self.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, default=Path(__file__).resolve().parents[1] / "dist/dump"
    )
    arguments = parser.parse_args()
    os.umask(0o077)
    Study(arguments.output).run()


if __name__ == "__main__":
    main()
