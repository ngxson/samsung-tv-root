import asyncio
from types import SimpleNamespace

import pytest

from samsung_tv_root import archive_root
from samsung_tv_root.sdb import CaptureResult


@pytest.mark.parametrize(
    ("persistent", "launcher", "cleaned", "unchanged", "error"),
    [
        (True, "timeout", True, True, None),
        (False, "timeout", True, True, TimeoutError),
        (True, "timeout", False, True, archive_root.ArchiveRootError),
        (True, "timeout", True, False, archive_root.ArchiveRootError),
        (True, "failed", True, True, archive_root.ArchiveRootError),
        (False, "success", True, True, None),
    ],
)
def test_launcher_timeout_never_bypasses_cleanup_or_original_file_checks(
    monkeypatch, tmp_path, persistent, launcher, cleaned, unchanged, error
):
    calls = []

    class Client:
        def connect(self):
            calls.append("reconnect")

        def capture(self, *args, **kwargs):
            return CaptureResult("[exit:1]" if launcher == "failed" else "[exit:0]", 0)

    class Connection:
        async def ping(self):
            return SimpleNamespace(
                uid=0,
                euid=0,
                gid=0,
                egid=0,
                effective_capabilities="3fffffffff",
                smack_label="User",
            )

        async def shutdown(self):
            calls.append("shutdown")

    class Server:
        listening_port = 12345
        secret = "test"

        def __init__(self, *args, **kwargs):
            pass

        async def start(self):
            pass

        async def accept(self, timeout):
            return Connection()

        async def close(self):
            pass

    def cleanup(*args):
        calls.append("cleanup")
        return cleaned

    def capture(*args):
        calls.append("hashes")
        return "original" if unchanged else "changed"

    for name in archive_root.PAYLOAD_FILES:
        (tmp_path / name).touch()
    monkeypatch.setattr(archive_root, "SdbClient", lambda *a, **kw: Client())
    monkeypatch.setattr(archive_root, "find_sdb", lambda: tmp_path / "sdb")
    monkeypatch.setattr(archive_root, "RootAgentServer", Server)
    monkeypatch.setattr(
        archive_root,
        "_check_target",
        lambda *a: SimpleNamespace(
            runtime="6.0", capabilities="capabilities", original_hashes="original"
        ),
    )
    monkeypatch.setattr(archive_root, "_prepare_artifacts", lambda *a: None)
    monkeypatch.setattr(archive_root, "_stage", lambda *a: None)
    monkeypatch.setattr(archive_root, "_cleanup", cleanup)
    monkeypatch.setattr(archive_root, "_capture", capture)
    real_wait_for = asyncio.wait_for

    async def wait_for(awaitable, timeout):
        if timeout == 15 and launcher == "timeout":
            return await real_wait_for(awaitable, 0)
        return await real_wait_for(awaitable, timeout)

    monkeypatch.setattr(asyncio, "wait_for", wait_for)

    async def session(connection):
        return 0

    async def run():
        return await archive_root.probe(
            "192.0.2.1",
            callback_host="192.0.2.2",
            payloads=tmp_path,
            session_handler=session if persistent else None,
        )

    if error:
        with pytest.raises(error):
            asyncio.run(run())
    else:
        evidence = asyncio.run(run())
        assert evidence["launcher_exit_observed"] == (launcher == "success")
        assert evidence["system_files_unchanged"] is True
        assert evidence["staging_cleaned"] is True
    assert calls.index("shutdown") < calls.index("cleanup") < calls.index("hashes")
    assert ("reconnect" in calls) == persistent
