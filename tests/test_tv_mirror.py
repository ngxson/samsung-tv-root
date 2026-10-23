import asyncio
from email.message import Message
import io
import json
from pathlib import Path
import queue
import re
import struct
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tv_mirror


def segment(marker, data):
    return bytes([255, marker]) + struct.pack(">H", len(data) + 2) + data


def jpeg(width=640, height=360):
    # Embedded EOI in an APP segment must not terminate the frame. Entropy
    # data also contains a stuffed FF byte and a restart marker.
    return (
        b"\xff\xd8"
        + segment(0xE1, b"metadata\xff\xd9")
        + segment(
            0xC0, b"\x08" + struct.pack(">HH", height, width) + b"\x01\x01\x11\x00"
        )
        + segment(0xDA, b"\x01\x01\x00\x00\x3f\x00")
        + b"abc\xff\x00def\xff\xd0ghi\xff\xd9"
    )


@pytest.mark.parametrize("chunk_size", [1, 2, 7, 4096])
def test_jpeg_stream_handles_network_splits_and_embedded_markers(chunk_size):
    frame = jpeg()
    stream = tv_mirror.JpegStream()
    wire = frame * 3
    frames = []
    for i in range(0, len(wire), chunk_size):
        frames.extend(stream.feed(wire[i : i + chunk_size]))
    assert frames == [frame] * 3
    assert tv_mirror.jpeg_dimensions(frames[0]) == (640, 360)
    assert not stream.buffer


def test_jpeg_stream_rejects_invalid_or_oversized_data(monkeypatch):
    with pytest.raises(ValueError, match="begin"):
        tv_mirror.JpegStream().feed(b"not an image")
    with pytest.raises(ValueError, match="length"):
        tv_mirror.JpegStream().feed(b"\xff\xd8\xff\xe0\x00\x00")
    monkeypatch.setattr(tv_mirror, "MAX_FRAME_BYTES", 32)
    with pytest.raises(ValueError, match="limit"):
        tv_mirror.JpegStream().feed(jpeg()[:-2] + b"a" * 40)


def post(viewer, payload, token=None, host="127.0.0.1:8765"):
    handler = object.__new__(viewer.handler())
    handler.server = SimpleNamespace(server_port=8765)
    handler.path = "/control"
    body = json.dumps(payload).encode()
    handler.headers = Message()
    handler.headers["Host"] = host
    handler.headers["Content-Length"] = str(len(body))
    if token is not None:
        handler.headers["X-Control-Token"] = token
    handler.connection = SimpleNamespace(settimeout=lambda value: None)
    handler.rfile = io.BytesIO(body)
    replies = []
    handler.json_reply = lambda code, value: replies.append((code, value))
    handler.do_POST()
    return replies[0]


def test_controls_require_token_connection_and_allowlisted_keys():
    viewer = tv_mirror.Viewer()
    assert post(viewer, {"key": "up"})[0] == 403
    assert post(viewer, {"key": "up"}, "é")[0] == 403
    assert post(viewer, {"key": "up"}, viewer.token)[0] == 503
    viewer.connected = True
    assert post(viewer, {"key": "up"}, viewer.token, "untrusted.test")[0] == 403
    for bad in ("Up; reboot", "", ["up"], {"key": "up"}, None):
        assert post(viewer, {"key": bad}, viewer.token)[0] == 400
    assert viewer.commands.empty()
    assert post(viewer, {"key": "up"}, viewer.token) == (202, {"id": 1, "key": "up"})
    assert viewer.commands.get_nowait() == (1, "up")


def test_control_queue_is_bounded():
    viewer = tv_mirror.Viewer()
    viewer.connected = True
    for _ in range(16):
        viewer.enqueue("1")
    with pytest.raises(queue.Full):
        viewer.enqueue("2")
    assert post(viewer, {"key": "2"}, viewer.token)[0] == 429


def test_every_visible_button_has_an_allowlisted_backend_action():
    buttons = re.findall(rb'data-key="([^"]+)"', tv_mirror.PAGE)
    assert len(buttons) == 20
    assert {button.decode() for button in buttons} == set(tv_mirror.KEYS)


@pytest.mark.parametrize(
    "key,native",
    [
        ("up", "Up"),
        ("ok", "Return"),
        ("home", "XF86Home"),
        ("volume_up", "XF86AudioRaiseVolume"),
        ("mute", "XF86AudioMute"),
        ("0", "0"),
        ("9", "9"),
    ],
)
def test_controls_dispatch_one_complete_keypress(key, native):
    async def run():
        viewer = tv_mirror.Viewer()
        viewer.connected = True
        stop = asyncio.Event()
        commands = []

        class Agent:
            async def execute(self, command, timeout):
                commands.append(command)
                stop.set()
                return SimpleNamespace(
                    exit_code=0, timed_out=False, stdout="", stderr=""
                )

        viewer.enqueue(key)
        await tv_mirror.dispatch_controls(Agent(), viewer, stop)
        assert commands == ["/usr/bin/enlightenment_input_key " + native]
        assert viewer.control == {"id": 1, "key": key, "error": None}

    asyncio.run(run())


def test_stream_cleanup_closes_clients_before_waiting_for_server(tmp_path):
    async def run():
        producers = []
        commands = []
        frame = jpeg()

        async def producer(port, token):
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(token.encode() + b"\n" + frame * 1000)
            try:
                await writer.drain()
                await reader.read()
            except (ConnectionResetError, BrokenPipeError):
                pass
            finally:
                writer.close()
                try:
                    await writer.wait_closed()
                except (ConnectionResetError, BrokenPipeError):
                    pass

        class Agent:
            identity = SimpleNamespace(pid=42)

            async def execute(self, command, timeout):
                commands.append(command)
                stdout = ""
                if "/usr/bin/setsid" in command:
                    port = int(re.search(r"/dev/tcp/127\.0\.0\.1/(\d+)", command)[1])
                    token = re.search(r"echo ([a-f0-9]{64})", command)[1]
                    producers.append(asyncio.create_task(producer(port, token)))
                    stdout = "424242\n"
                return SimpleNamespace(
                    exit_code=0, timed_out=False, stdout=stdout, stderr=""
                )

        args = SimpleNamespace(
            host="127.0.0.1",
            callback_host="127.0.0.1",
            width=640,
            height=360,
            fps=4,
            quality=65,
            frames=1,
        )
        viewer = tv_mirror.Viewer()
        try:
            async with asyncio.timeout(3):
                assert (
                    await tv_mirror.capture_session(
                        Agent(), viewer, asyncio.Event(), args, tmp_path
                    )
                    == 0
                )
                await asyncio.gather(*producers)
        finally:
            for task in producers:
                task.cancel()
            await asyncio.gather(*producers, return_exceptions=True)
        assert not viewer.connected
        assert (tmp_path / "latest.jpg").read_bytes() == frame
        assert any("kill -INT 424242" in command for command in commands)

    asyncio.run(run())
