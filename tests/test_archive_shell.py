import asyncio
from pathlib import Path
from types import SimpleNamespace

import pytest

from samsung_tv_root import archive_root
from samsung_tv_root.sdb import CaptureResult


@pytest.mark.parametrize("fail_session", (False, True))
def test_archive_shell_shuts_down_and_cleans_up_on_exit_or_failure(
    monkeypatch, tmp_path, fail_session
):
    events = []

    class Agent:
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
            events.append("shutdown")

    agent = Agent()

    class Server:
        listening_port = 12345
        secret = b"a" * 32

        def __init__(self, *args, **kwargs):
            pass

        async def start(self):
            pass

        async def accept(self, timeout):
            return agent

        async def close(self):
            events.append("close")

    class Client:
        def __init__(self, *args, **kwargs):
            pass

        def connect(self):
            events.append("reconnect")

        def capture(self, command, **kwargs):
            assert kwargs["persistent"] is True
            events.append("launch")
            return CaptureResult("\n[exit:0]\n", 1)

    monkeypatch.setattr(archive_root, "RootAgentServer", Server)
    monkeypatch.setattr(archive_root, "SdbClient", Client)
    monkeypatch.setattr(archive_root, "find_sdb", lambda: Path("sdb"))
    monkeypatch.setattr(
        archive_root,
        "_check_target",
        lambda client: archive_root.TargetAssessment("capabilities", "hashes", "6.0"),
    )
    monkeypatch.setattr(archive_root, "_prepare_artifacts", lambda *args: None)
    monkeypatch.setattr(archive_root, "_stage", lambda *args: None)
    monkeypatch.setattr(archive_root, "_capture", lambda *args: "hashes")

    def cleanup(*args):
        events.append("cleanup")
        return True

    monkeypatch.setattr(archive_root, "_cleanup", cleanup)
    for filename in archive_root.PAYLOAD_FILES:
        (tmp_path / filename).touch()

    async def session(connection):
        assert connection is agent
        events.append("session")
        if fail_session:
            raise RuntimeError("terminal failed")
        return 0

    call = archive_root.probe(
        "192.0.2.50",
        callback_host="192.0.2.10",
        payloads=tmp_path,
        session_handler=session,
    )
    if fail_session:
        with pytest.raises(RuntimeError, match="terminal failed"):
            asyncio.run(call)
    else:
        evidence = asyncio.run(call)
        assert evidence["session_exit"] == 0
        assert evidence["staging_cleaned"]
        assert evidence["system_files_unchanged"]
    assert events.index("session") < events.index("shutdown") < events.index("cleanup")
    assert events.index("reconnect") < events.index("cleanup")
    assert "close" in events
