import asyncio
import gzip
from pathlib import Path
import shlex
import re
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from dump_tv import Acquisition


@pytest.mark.parametrize("fail", (False, True))
@pytest.mark.parametrize("compress", (False, True))
def test_dump_stream_verifies_content_and_resumes_only_successful_files(
    tmp_path, fail, compress
):
    acquisition = Acquisition(tmp_path / "dump", "127.0.0.1", "127.0.0.1")
    temporary = tmp_path / "remote"
    commands = []
    processes = []

    class Agent:
        identity = SimpleNamespace(pid=123)

        async def execute(self, command, timeout):
            commands.append(command)
            command = command.replace("/run/samsung-tv-root/agent-123", str(temporary))
            if "/usr/bin/setsid /bin/bash -c" in command:
                parts = shlex.split(command)
                inner = parts[parts.index("-c") + 1].replace(
                    "sha256sum", "shasum -a 256"
                )
                inner = re.sub(
                    r"/opt/usr/apps/com.samsung.tv.iot-hub-control/bin/socat -u -b 1048576 STDIN TCP:([^:]+):(\d+),nodelay,sndbuf=1048576",
                    lambda match: "/bin/bash -c "
                    + shlex.quote(f"exec 3<>/dev/tcp/{match[1]}/{match[2]}; cat >&3"),
                    inner,
                )
                directory = Path(parts[2])
                directory.mkdir(parents=True)
                log = (directory / "log").open("wb")
                process = subprocess.Popen(
                    ["/bin/bash", "-c", inner], stdout=log, stderr=log
                )
                log.close()
                processes.append(process)
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
                stdout=result.stdout,
                stderr=result.stderr,
                timed_out=False,
                exit_code=result.returncode,
            )

    async def run():
        producer = "printf dump-test" + ("; exit 7" if fail else "")
        if fail:
            with pytest.raises(RuntimeError, match="source_exit=7"):
                await acquisition.transfer(
                    Agent(),
                    "archives/test.bin",
                    producer,
                    {"kind": "test"},
                    compress=compress,
                )
            assert "archives/test.bin" not in acquisition.manifest["artifacts"]
            assert not (tmp_path / "dump/archives/test.bin").exists()
        else:
            await acquisition.transfer(
                Agent(),
                "archives/test.bin",
                producer,
                {"kind": "test"},
                compress=compress,
            )
            content = (tmp_path / "dump/archives/test.bin").read_bytes()
            assert (gzip.decompress(content) if compress else content) == b"dump-test"
            count = len(commands)
            await acquisition.transfer(
                Agent(),
                "archives/test.bin",
                producer,
                {"kind": "test"},
                compress=compress,
            )
            assert len(commands) == count

    try:
        asyncio.run(run())
    finally:
        for process in processes:
            process.wait(timeout=5)
