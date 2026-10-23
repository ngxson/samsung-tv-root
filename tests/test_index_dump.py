import gzip
import hashlib
import io
import json
from pathlib import Path
import sqlite3
import sys
import tarfile

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from index_dump import Study, archive_path


def fixture_dump(tmp_path):
    output = tmp_path / "dump"
    (output / "archives").mkdir(parents=True)
    (output / "metadata").mkdir()
    (output / "metadata/block-layout.txt").write_text("mmcblk0p1 8 16\n")
    manifest = {"started": "test", "artifacts": {}, "errors": [], "limitations": []}
    return output, manifest


def record(output, manifest, name, content, **details):
    path = output / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    manifest["artifacts"][name] = {
        "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
        "source_exit": 0,
        **details,
    }
    (output / "manifest.json").write_text(json.dumps(manifest))


@pytest.mark.parametrize("compressed", (False, True))
def test_index_preserves_contents_metadata_collisions_and_links_without_following_them(
    tmp_path,
    compressed,
):
    output, manifest = fixture_dump(tmp_path)
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz" if compressed else "w") as tar:
        for name, content in [
            ("./etc/config", b"first"),
            ("./etc/CONFIG", b"second"),
            ("./bin/tool", b"\x7fELFtest"),
        ]:
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.uid, info.gid, info.mode = 123, 456, 0o4755
            info.pax_headers = {"SCHILY.xattr.security.SMACK64": "System"}
            tar.addfile(info, io.BytesIO(content))
        link = tarfile.TarInfo("./outside")
        link.type, link.linkname = tarfile.SYMTYPE, str(tmp_path / "outside")
        tar.addfile(link)
        content = b"safe"
        nested = tarfile.TarInfo("./outside/file")
        nested.size = len(content)
        tar.addfile(nested, io.BytesIO(content))
    record(
        output,
        manifest,
        "archives/root.tar.gz" if compressed else "archives/root.tar",
        buffer.getvalue(),
        kind="filesystem",
        mount="/",
    )
    for offset, content in [(0, b"abc"), (3, b"def")]:
        record(
            output,
            manifest,
            f"images/device/{offset}.img.gz",
            gzip.compress(content),
            kind="block-device",
            device="/dev/device",
            device_bytes=6,
            offset=offset,
            uncompressed_bytes=3,
        )
    Study(output).run()
    assert (output / "rootfs/etc/config").read_bytes() == b"first"
    assert not (tmp_path / "outside").exists()
    assert (output / "rootfs/outside/file").read_bytes() == b"safe"
    assert (output / "images/device.img").read_bytes() == b"abcdef"
    with sqlite3.connect(output / "index.sqlite") as db:
        row = db.execute(
            "SELECT local_path, uid, gid, mode, pax_headers FROM files WHERE path='/etc/CONFIG'"
        ).fetchone()
        assert (output / row[0]).read_bytes() == b"second"
        assert row[1:4] == (123, 456, 0o4755)
        assert json.loads(row[4])["SCHILY.xattr.security.SMACK64"] == "System"
        assert (
            db.execute("SELECT format FROM files WHERE path='/bin/tool'").fetchone()[0]
            == "ELF"
        )
        assert (
            db.execute("SELECT kind FROM files WHERE path='/outside'").fetchone()[0]
            == "symlink"
        )
    assert len(json.loads((output / "collisions.json").read_text())) == 1


@pytest.mark.parametrize("name", ["../escape", "/absolute", "a/../../escape"])
def test_rejects_archive_path_traversal(name):
    with pytest.raises(ValueError, match="Unsafe archive member"):
        archive_path("/opt", name)


def test_refuses_preexisting_host_symlinks(tmp_path):
    output, manifest = fixture_dump(tmp_path)
    (output / "manifest.json").write_text(json.dumps(manifest))
    study = Study(output)
    (output / "rootfs/escape").symlink_to(tmp_path)
    with pytest.raises(ValueError, match="Refusing local symlink"):
        study.destination("/escape/file")
    study.db.close()


def test_rejects_corrupted_acquisition(tmp_path):
    output, manifest = fixture_dump(tmp_path)
    record(
        output,
        manifest,
        "archives/root.tar.gz",
        b"expected",
        kind="filesystem",
        mount="/",
    )
    study = Study(output)
    (output / "archives/root.tar.gz").write_bytes(b"corrupt!")
    with pytest.raises(ValueError, match="SHA-256 mismatch"):
        study.filesystem(
            "archives/root.tar.gz", manifest["artifacts"]["archives/root.tar.gz"]
        )
    study.db.close()


def test_reassembles_and_verifies_split_usb_archives(tmp_path):
    output, manifest = fixture_dump(tmp_path)
    content = b"whole compressed stream"
    record(
        output, manifest, "archives/test.tar.gz", content, kind="filesystem", mount="/"
    )
    parts = []
    for number, data in enumerate((content[:7], content[7:])):
        name = f"archives/test.tar.gz.part-{number:04}"
        (output / name).write_bytes(data)
        parts.append(
            {
                "name": name,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    details = manifest["artifacts"]["archives/test.tar.gz"]
    details["parts"] = parts
    (output / "archives/test.tar.gz").unlink()
    study = Study(output)
    assert study.verify("archives/test.tar.gz", details).read_bytes() == content
    (output / parts[0]["name"]).write_bytes(b"corrupt")
    with pytest.raises(ValueError, match="part verification failed"):
        study.verify("archives/test.tar.gz", details)
    study.db.close()
