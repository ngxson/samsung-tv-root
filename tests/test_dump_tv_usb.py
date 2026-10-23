import asyncio
import gzip
from pathlib import Path
import shlex
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from dump_tv_usb import UsbAcquisition, size_command


def test_usb_size_counter_preserves_large_byte_counts(tmp_path):
    expected = 0
    paths = []
    for i, size in enumerate((1073741824, 1073741824, 123456789)):
        path = tmp_path / f"part-{i}"
        with path.open("wb") as stream:
            stream.truncate(size)
        paths.append(str(path))
        expected += size
    command = size_command(shlex.join(paths))
    if sys.platform == "darwin":
        command = command.replace("stat -c %s", "stat -f %z")
    output = subprocess.check_output(["/bin/bash", "-c", command], text=True)
    assert output == f"{expected}\n"


@pytest.mark.parametrize("fail", (False, True))
@pytest.mark.parametrize("uncompressed", (False, True))
@pytest.mark.parametrize("escaped_transport", (False, True))
def test_usb_capture_verifies_readback_and_reuses_only_verified_artifacts(
    tmp_path, fail, uncompressed, escaped_transport
):
    mount = "/opt/media/USBDriveA1"
    usb = tmp_path / "usb"
    usb.mkdir()
    acquisition = UsbAcquisition(
        tmp_path / "records",
        "192.0.2.1",
        "192.0.2.2",
        mount,
        "samsung-tv-dump-test",
        uncompressed,
    )
    acquisition.device = usb.stat().st_dev
    processes = []
    commands = []

    class Agent:
        identity = SimpleNamespace(pid=123)

        async def write_file(self, path, data, timeout=60):
            local = Path(
                path.replace(
                    "/run/samsung-tv-root/agent-123", str(tmp_path / "runtime")
                )
            )
            local.parent.mkdir(parents=True, exist_ok=True)
            with local.open("xb") as stream:
                stream.write(data)

        async def execute(self, command, timeout):
            commands.append(command)
            if escaped_transport:
                # The TV's released agent doubles backslashes when passing its
                # command through ProcessStartInfo.Arguments on this runtime.
                command = command.replace("\\", "\\\\")
            if command == "sync":
                return SimpleNamespace(
                    stdout="", stderr="", timed_out=False, exit_code=0
                )
            command = command.replace("/proc/1/root" + mount, str(usb)).replace(
                "/run/samsung-tv-root/agent-123", str(tmp_path / "runtime")
            )
            command = (
                command.replace("sha256sum", "shasum -a 256")
                .replace("stat -c %d", "stat -f %d")
                .replace("stat -c %s", "stat -f %z")
            )
            # The test producer is smaller than one part; macOS split lacks
            # the TV's GNU numeric-suffix options.
            command = command.replace("| split -b 1073741824 -d -a 4 - ", ">")
            command = command.replace(".partial-part-; rc=", ".partial-part-0000; rc=")
            if "/usr/bin/setsid /bin/bash -c" in command:
                parts = shlex.split(command)
                directory = Path(parts[2])
                directory.mkdir(parents=True)
                with (directory / "log").open("wb") as log:
                    processes.append(
                        subprocess.Popen(
                            ["/bin/bash", "-c", parts[parts.index("-c") + 1]],
                            stdout=log,
                            stderr=log,
                        )
                    )
                return SimpleNamespace(
                    stdout="", stderr="", timed_out=False, exit_code=0
                )
            result = await asyncio.to_thread(
                subprocess.run,
                ["/bin/bash", "-c", command],
                capture_output=True,
                text=True,
                timeout=timeout,
            )
            return SimpleNamespace(
                stdout=result.stdout.replace(str(usb), "/proc/1/root" + mount),
                stderr=result.stderr,
                timed_out=False,
                exit_code=result.returncode,
            )

    async def run():
        producer = "printf content | gzip -1" + ("; exit 7" if fail else "")
        name = "archives/test.tar.gz"
        stored_name = name.removesuffix(".gz") if uncompressed else name
        stale = usb / "samsung-tv-dump-test" / (stored_name + ".partial-part-0009")
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b"interrupted prior attempt")
        if fail:
            with pytest.raises(RuntimeError, match="source_exit=7"):
                await acquisition.transfer(
                    Agent(), name, producer, {"kind": "filesystem"}
                )
            assert name not in acquisition.manifest["artifacts"]
        else:
            await acquisition.transfer(Agent(), name, producer, {"kind": "filesystem"})
            stored_name = name.removesuffix(".gz") if uncompressed else name
            metadata = acquisition.manifest["artifacts"][stored_name]
            content = b"".join(
                (usb / "samsung-tv-dump-test" / part["name"]).read_bytes()
                for part in metadata["parts"]
            )
            assert (content if uncompressed else gzip.decompress(content)) == b"content"
            assert metadata["sha256"] == metadata["source_sha256"]
            previous = len(commands)
            await acquisition.transfer(Agent(), name, producer, {"kind": "filesystem"})
            assert len(commands) == previous + 1
            await acquisition.publish_metadata(Agent())
            assert (usb / "samsung-tv-dump-test/manifest.json").read_bytes() == (
                tmp_path / "records/manifest.json"
            ).read_bytes()
            assert not (tmp_path / "records" / name).exists()

    try:
        asyncio.run(run())
    finally:
        for process in processes:
            process.wait(timeout=5)


def test_usb_prepare_preserves_folder_identity_and_repairs_legacy_marker(tmp_path):
    mount = "/opt/media/USBDriveA1"
    usb = tmp_path / "usb"
    usb.mkdir()
    acquisition = UsbAcquisition(
        tmp_path / "records", "192.0.2.1", "192.0.2.2", mount, "samsung-tv-dump-test"
    )

    class Agent:
        async def execute(self, command, timeout):
            if command == "cat /proc/1/mountinfo":
                output = f"20 1 8:1 / {mount} rw - vfat /dev/sda1 rw\n"
            elif command.startswith("stat -c %d"):
                output = "42\n"
            elif command.startswith("df -Pk"):
                output = "Filesystem 1024-blocks Used Available Capacity Mounted\n/dev/sda1 60000000 0 60000000 0% /usb\n"
            else:
                command = command.replace("/proc/1/root" + mount, str(usb))
                r = subprocess.run(
                    ["/bin/bash", "-c", command], capture_output=True, text=True
                )
                return SimpleNamespace(
                    stdout=r.stdout,
                    stderr=r.stderr,
                    timed_out=False,
                    exit_code=r.returncode,
                )
            return SimpleNamespace(
                stdout=output, stderr="", timed_out=False, exit_code=0
            )

    async def run():
        agent = Agent()
        await acquisition.prepare(agent, 15)
        marker = usb / "samsung-tv-dump-test/.acquisition-token"
        assert marker.read_text() == acquisition.manifest["usb_token"]
        await acquisition.prepare(agent, 15)
        marker.write_text(acquisition.manifest["usb_token"] + "\\n")
        await acquisition.prepare(agent, 15)
        assert marker.read_text() == acquisition.manifest["usb_token"]
        marker.write_text("another acquisition")
        with pytest.raises(RuntimeError, match="another acquisition"):
            await acquisition.prepare(agent, 15)

    asyncio.run(run())
