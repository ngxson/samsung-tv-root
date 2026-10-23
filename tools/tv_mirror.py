#!/usr/bin/env python3
"""Local Q7F screen stream and remote control through temporary archive root."""

from __future__ import annotations

import argparse
import asyncio
from collections import deque
from dataclasses import dataclass
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path
import queue
import secrets
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
from urllib.parse import urlsplit
import webbrowser

from archive_shell import REPOSITORY, configure_payloads
from samsung_tv_root.archive_root import ArchiveRootError, probe
from samsung_tv_root.root_agent import RootAgentError
from samsung_tv_root.sdb import SdbClient, SdbError, route_callback_host


MAX_FRAME_BYTES = 4 * 1024 * 1024
KEYS = {
    "up": "Up",
    "down": "Down",
    "left": "Left",
    "right": "Right",
    "ok": "Return",
    "back": "XF86Back",
    "home": "XF86Home",
    "volume_up": "XF86AudioRaiseVolume",
    "volume_down": "XF86AudioLowerVolume",
    "mute": "XF86AudioMute",
    **{str(n): str(n) for n in range(10)},
}
PAGE = Path(__file__).with_suffix(".html").read_bytes()


@dataclass(frozen=True)
class Frame:
    data: bytes
    metadata: dict


def jpeg_end(data: bytes | bytearray) -> int | None:
    """Find a complete JPEG, respecting segment lengths and entropy byte stuffing."""
    if len(data) < 2:
        return None
    if data[:2] != b"\xff\xd8":
        raise ValueError("Stream did not begin with a JPEG")
    pos, entropy = 2, False
    while pos < len(data):
        if entropy:
            marker_pos = data.find(b"\xff", pos)
            if marker_pos < 0:
                return None
            pos = marker_pos
        elif data[pos] != 255:
            raise ValueError("Invalid JPEG marker")
        while pos < len(data) and data[pos] == 255:
            pos += 1
        if pos == len(data):
            return None
        marker = data[pos]
        pos += 1
        if entropy and (marker == 0 or 0xD0 <= marker <= 0xD7):
            continue
        if marker == 0xD9:
            return pos
        if marker == 0xD8 or marker == 0:
            raise ValueError("Invalid JPEG segment")
        entropy = False
        if marker == 1 or 0xD0 <= marker <= 0xD7:
            continue
        if len(data) < pos + 2:
            return None
        length = int.from_bytes(data[pos : pos + 2], "big")
        if length < 2:
            raise ValueError("Invalid JPEG segment length")
        if len(data) < pos + length:
            return None
        pos += length
        entropy = marker == 0xDA
    return None


class JpegStream:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        self.buffer.extend(data)
        frames = []
        while self.buffer:
            end = jpeg_end(self.buffer)
            if end is None:
                if len(self.buffer) > MAX_FRAME_BYTES:
                    raise ValueError("JPEG frame exceeds transfer limit")
                break
            if end > MAX_FRAME_BYTES:
                raise ValueError("JPEG frame exceeds transfer limit")
            frames.append(bytes(self.buffer[:end]))
            del self.buffer[:end]
        return frames


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    if data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError("Incomplete JPEG")
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 255:
            break
        while pos < len(data) and data[pos] == 255:
            pos += 1
        if pos == len(data):
            break
        marker = data[pos]
        pos += 1
        if marker in (0xDA, 0xD9):
            break
        length = int.from_bytes(data[pos : pos + 2], "big")
        if length < 2 or pos + length > len(data):
            break
        if marker in (0xC0, 0xC1, 0xC2) and length >= 8:
            height, width = struct.unpack(">HH", data[pos + 3 : pos + 7])
            if 0 < width <= 4096 and 0 < height <= 2160:
                return width, height
            break
        pos += length
    raise ValueError("JPEG has no supported dimensions")


class Viewer:
    def __init__(self, target_fps=4):
        self.frame: Frame | None = None
        self.message = "Connecting to TV..."
        self.target_fps = target_fps
        self.connected = False
        self.closed = False
        self.token = secrets.token_hex(32)
        self.condition = threading.Condition()
        self.commands = queue.Queue(maxsize=16)
        self.control_id = 0
        self.control = None
        self.arrivals = deque(maxlen=40)
        self.first_arrival = None
        self.frame_count = 0
        self.byte_count = 0

    def publish(self, data):
        width, height = jpeg_dimensions(data)
        now = time.monotonic()
        with self.condition:
            self.frame_count += 1
            self.byte_count += len(data)
            if self.first_arrival is None:
                self.first_arrival = now
            self.arrivals.append(now)
            span = now - self.arrivals[0]
            fps = (len(self.arrivals) - 1) / span if span > 0 else 0.0
            self.frame = Frame(
                data,
                {
                    "sequence": self.frame_count,
                    "width": width,
                    "height": height,
                    "bytes": len(data),
                    "received_at": time.time(),
                    "fps": fps,
                    "target_fps": self.target_fps,
                },
            )
            self.condition.notify_all()

    def status(self):
        with self.condition:
            status = {
                "sequence": 0,
                "message": self.message,
                "connected": self.connected,
                "control": self.control,
            }
            if self.frame:
                status.update(self.frame.metadata)
            return status

    def enqueue(self, key):
        with self.condition:
            if not self.connected or self.closed:
                raise RuntimeError("TV is not connected")
            if not isinstance(key, str) or key not in KEYS:
                raise ValueError("Unknown remote button")
            identifier = self.control_id + 1
            self.commands.put_nowait((identifier, key))
            self.control_id = identifier
            return identifier

    def close(self):
        with self.condition:
            self.closed = True
            self.connected = False
            self.condition.notify_all()

    def handler(self):
        viewer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def allowed(self):
                hosts = {
                    f"127.0.0.1:{self.server.server_port}",
                    f"localhost:{self.server.server_port}",
                }
                return (
                    self.headers.get("Host") in hosts
                    and self.headers.get("Sec-Fetch-Site") != "cross-site"
                )

            def reply(self, code, content_type, body):
                self.send_response(code)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Cross-Origin-Resource-Policy", "same-origin")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def json_reply(self, code, value):
                self.reply(code, "application/json", json.dumps(value).encode())

            def do_GET(self):
                if not self.allowed():
                    self.send_error(403)
                    return
                path = urlsplit(self.path).path
                if path == "/":
                    self.reply(
                        200,
                        "text/html; charset=utf-8",
                        PAGE.replace(b"__CONTROL_TOKEN__", viewer.token.encode()),
                    )
                elif path == "/status":
                    self.json_reply(200, viewer.status())
                elif path == "/frame.jpg" and viewer.frame:
                    self.reply(200, "image/jpeg", viewer.frame.data)
                elif path == "/stream.mjpg":
                    self.stream()
                else:
                    self.send_error(404)

            def stream(self):
                self.connection.settimeout(10)
                self.send_response(200)
                self.send_header(
                    "Content-Type", "multipart/x-mixed-replace; boundary=frame"
                )
                self.send_header("Cache-Control", "no-store")
                self.send_header("Cross-Origin-Resource-Policy", "same-origin")
                self.end_headers()
                sequence = 0
                try:
                    while not viewer.closed:
                        with viewer.condition:
                            viewer.condition.wait_for(
                                lambda sequence=sequence: viewer.closed
                                or (
                                    viewer.frame is not None
                                    and viewer.frame.metadata["sequence"] != sequence
                                ),
                                timeout=2,
                            )
                            if viewer.closed:
                                return
                            frame = viewer.frame
                        if frame is None or frame.metadata["sequence"] == sequence:
                            continue
                        sequence = frame.metadata["sequence"]
                        header = (
                            "--frame\r\nContent-Type: image/jpeg\r\n"
                            f"Content-Length: {len(frame.data)}\r\n\r\n"
                        ).encode()
                        self.wfile.write(header + frame.data + b"\r\n")
                        self.wfile.flush()
                except (OSError, TimeoutError):
                    pass

            def do_POST(self):
                if not self.allowed() or not hmac.compare_digest(
                    self.headers.get("X-Control-Token", "").encode(),
                    viewer.token.encode(),
                ):
                    self.json_reply(403, {"error": "Invalid control token or origin"})
                    return
                if urlsplit(self.path).path != "/control":
                    self.json_reply(404, {"error": "Not found"})
                    return
                try:
                    size = int(self.headers.get("Content-Length", "0"))
                    if not 0 < size <= 256:
                        raise ValueError("Invalid control request size")
                    self.connection.settimeout(3)
                    value = json.loads(self.rfile.read(size))
                    if not isinstance(value, dict) or set(value) != {"key"}:
                        raise ValueError("Expected a remote button")
                    identifier = viewer.enqueue(value["key"])
                    self.json_reply(202, {"id": identifier, "key": value["key"]})
                except (ValueError, UnicodeError) as error:
                    self.json_reply(400, {"error": str(error)})
                except queue.Full:
                    self.json_reply(
                        429, {"error": "Remote queue is full; try again shortly"}
                    )
                except RuntimeError as error:
                    self.json_reply(503, {"error": str(error)})
                except (OSError, TimeoutError):
                    pass

        return Handler


async def execute(agent, command: str, timeout: float = 20) -> str:
    result = await agent.execute(command, timeout=timeout)
    if result.timed_out or result.exit_code:
        raise RuntimeError(
            f"TV command failed ({result.exit_code}, timeout={result.timed_out}): "
            f"{result.stderr[-2000:]}\n{result.stdout[-2000:]}"
        )
    return result.stdout


def stream_command(args, callback, port, token, runtime):
    # Every shell value is either validated or quoted. fd 3 belongs to the SDK
    # launch transport and must not be inherited by the background producer.
    caps = (
        f"video/x-raw,format=BGRA,width={args.width},height={args.height},"
        f"framerate={args.fps}/1"
    )
    producer = (
        f"exec 4<>/dev/tcp/{callback}/{port}; "
        f"echo {shlex.quote(token)} >&4; "
        f"exec env XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0 "
        f"GST_REGISTRY={shlex.quote(runtime + '/gst-registry.bin')} "
        f"gst-launch-1.0 -q -e waylandsrc ! {shlex.quote(caps)} "
        f"! videoconvert ! video/x-raw,format=I420 ! jpegenc quality={args.quality} "
        "! fdsink fd=4 sync=false"
    )
    return (
        f"/usr/bin/setsid /bin/bash -c {shlex.quote(producer)} "
        f"3>&- </dev/null >{runtime}/stream.log 2>&1 & p=$!; "
        f"echo $p >{runtime}/stream.pid; echo $p"
    )


async def dispatch_controls(agent, viewer, stop):
    while not stop.is_set():
        try:
            identifier, key = viewer.commands.get_nowait()
        except queue.Empty:
            await asyncio.sleep(0.025)
            continue
        error = None
        try:
            await execute(
                agent,
                "/usr/bin/enlightenment_input_key " + shlex.quote(KEYS[key]),
                timeout=5,
            )
        except (RootAgentError, RuntimeError) as exc:
            error = str(exc)
        with viewer.condition:
            viewer.control = {"id": identifier, "key": key, "error": error}
        viewer.commands.task_done()


async def receive_frames(reader, viewer, stop, args):
    parser = JpegStream()
    while not stop.is_set():
        data = await asyncio.wait_for(reader.read(65536), timeout=10)
        if not data:
            raise RuntimeError("TV capture stream disconnected")
        for frame in parser.feed(data):
            if jpeg_dimensions(frame) != (args.width, args.height):
                raise RuntimeError(
                    "TV stream dimensions do not match the requested resolution"
                )
            viewer.publish(frame)
            if viewer.frame_count == 1 or viewer.frame_count % (args.fps * 5) == 0:
                info = viewer.frame.metadata
                print(
                    f"Frame {info['sequence']}: {info['width']}x{info['height']}, "
                    f"{info['bytes'] / 1024:.0f} KiB, {info['fps']:.2f} fps",
                    flush=True,
                )
            if args.frames and viewer.frame_count >= args.frames:
                stop.set()
                return


async def stop_producer(agent, pid):
    if pid is None:
        return
    # Only signal our recorded producer, and check its identity in case it exited.
    command = (
        f"if grep -aq gst-launch-1.0 /proc/{pid}/cmdline 2>/dev/null; then "
        f"kill -INT {pid}; "
        f"for i in 1 2 3 4 5 6 7 8 9 10; do "
        f"kill -0 {pid} 2>/dev/null || break; sleep 0.1; done; "
        f"if grep -aq gst-launch-1.0 /proc/{pid}/cmdline 2>/dev/null; then kill -TERM {pid}; fi; fi"
    )
    await execute(agent, command, timeout=5)


async def capture_session(agent, viewer, stop, args, output):
    runtime = f"/run/samsung-tv-root/agent-{agent.identity.pid}"
    token = secrets.token_hex(32)
    connection = asyncio.get_running_loop().create_future()
    clients, handlers = set(), set()
    tasks = []
    controls_task = None
    pid = None

    async def accept(reader, writer):
        task = asyncio.current_task()
        handlers.add(task)
        clients.add(writer)
        try:
            peer = writer.get_extra_info("peername")
            if not peer or peer[0] != args.host:
                return
            line = await asyncio.wait_for(reader.readline(), timeout=3)
            if (
                not hmac.compare_digest(line.strip(), token.encode())
                or connection.done()
            ):
                return
            connection.set_result((reader, writer))
            await writer.wait_closed()
        except (OSError, ValueError, asyncio.TimeoutError):
            pass
        finally:
            writer.close()
            clients.discard(writer)
            handlers.discard(task)

    server = await asyncio.start_server(accept, args.callback_host, 0, limit=4096)
    port = server.sockets[0].getsockname()[1]
    try:
        await execute(agent, f"mkdir -p {runtime}")
        pid_text = await execute(
            agent, stream_command(args, args.callback_host, port, token, runtime)
        )
        pid = int(pid_text.strip())
        if pid <= 1:
            raise RuntimeError("Invalid capture producer PID")
        try:
            reader, writer = await asyncio.wait_for(connection, timeout=20)
        except TimeoutError as error:
            log = await execute(agent, f"tail -c 6000 {runtime}/stream.log")
            raise RuntimeError("TV stream did not connect: " + log) from error
        viewer.connected = True
        viewer.message = "Connected; waiting for video..."
        frame_task = asyncio.create_task(receive_frames(reader, viewer, stop, args))
        controls_task = asyncio.create_task(dispatch_controls(agent, viewer, stop))
        stop_task = asyncio.create_task(stop.wait())
        tasks = [frame_task, controls_task, stop_task]
        finished, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in finished:
            await task
        return 0
    finally:
        viewer.connected = False
        stop.set()
        # Let any in-flight key command finish; cancelling it invalidates the agent.
        for task in tasks:
            if task is not controls_task:
                task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        server.close()
        for writer in list(clients):
            writer.close()
        for task in list(handlers):
            task.cancel()
        if handlers:
            await asyncio.gather(*list(handlers), return_exceptions=True)
        await server.wait_closed()
        await stop_producer(agent, pid)
        log = await execute(agent, f"tail -c 6000 {runtime}/stream.log") if pid else ""
        (output / "stream.log").write_text(log)
        if viewer.frame:
            (output / "latest.jpg").write_bytes(viewer.frame.data)
            metadata = dict(viewer.frame.metadata)
            metadata["total_frames"] = viewer.frame_count
            metadata["total_bytes"] = viewer.byte_count
            (output / "latest.json").write_text(json.dumps(metadata, indent=2) + "\n")


async def run(args, server, viewer, output):
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()

    def request_stop():
        if not stop.is_set():
            print("Stopping the stream and cleaning up the TV session...", flush=True)
        stop.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, request_stop)
    args.callback_host = args.callback_host or route_callback_host(args.host)
    url = f"http://127.0.0.1:{server.server_port}"
    print(
        f"TV viewer: {url}\nTarget: {args.width}x{args.height} at {args.fps} fps. Press Ctrl-C to stop.",
        flush=True,
    )
    if not args.no_open:
        webbrowser.open(url)
    client = SdbClient(Path(os.environ["SDB"]), args.host, timeout=15)
    try:
        evidence = await probe(
            args.host,
            callback_host=args.callback_host,
            session_handler=lambda agent: capture_session(
                agent, viewer, stop, args, output
            ),
        )
        (output / "last-session.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(
            "TV session closed; staging removed and system-file hashes verified.",
            flush=True,
        )
        return int(evidence["session_exit"])
    finally:
        viewer.close()
        try:
            await asyncio.to_thread(client.disconnect)
        except SdbError as error:
            print(f"SDB disconnect failed: {error}", file=sys.stderr)
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.remove_signal_handler(sig)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "host",
        nargs="?",
        default=os.environ.get("TV_HOST"),
        help="TV IPv4 address (or set TV_HOST)",
    )
    parser.add_argument(
        "--callback-host", help="Mac IPv4 address (auto-detected by default)"
    )
    parser.add_argument("--port", type=int, default=8765, help="Local browser port")
    parser.add_argument(
        "--width", type=int, default=640, help="Stream width (default: 640)"
    )
    parser.add_argument(
        "--height", type=int, default=360, help="Stream height (default: 360)"
    )
    parser.add_argument(
        "--fps", type=int, default=4, help="Target frames per second (default: 4)"
    )
    parser.add_argument(
        "--quality", type=int, default=65, help="JPEG quality 1–95 (default: 65)"
    )
    parser.add_argument(
        "--frames", type=int, default=0, help="Stop after N frames; 0 runs until Ctrl-C"
    )
    parser.add_argument(
        "--no-open", action="store_true", help="Do not open a browser automatically"
    )
    parser.add_argument(
        "--container", default=os.environ.get("SAMSUNG_TV_SDB_CONTAINER", "tizen-sdb")
    )
    args = parser.parse_args()
    if not args.host:
        parser.error("provide the TV host argument or set TV_HOST")
    ipaddress.IPv4Address(args.host)
    if args.callback_host:
        ipaddress.IPv4Address(args.callback_host)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if (
        not (160 <= args.width <= 1920 and 90 <= args.height <= 1080)
        or args.width % 2
        or args.height % 2
    ):
        parser.error("Use even dimensions between 160x90 and 1920x1080")
    if not 1 <= args.fps <= 15:
        parser.error("--fps must be between 1 and 15")
    if not 1 <= args.quality <= 95:
        parser.error("--quality must be between 1 and 95")
    if args.frames < 0:
        parser.error("--frames must be nonnegative")
    docker = shutil.which("docker")
    if docker is None:
        parser.error("Docker is not installed or is not on PATH")
    configure_payloads()
    os.environ["SAMSUNG_TV_SDB_CONTAINER"] = args.container
    os.environ["SDB"] = str(REPOSITORY / "tools/sdb_docker.py")
    output = REPOSITORY / "build/tv-mirror"
    output.mkdir(parents=True, exist_ok=True)
    viewer = Viewer(args.fps)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), viewer.handler())
    (output / "last-session.json").unlink(missing_ok=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run(
            [docker, "start", args.container], check=True, stdout=subprocess.DEVNULL
        )
        return asyncio.run(run(args, server, viewer, output))
    finally:
        viewer.close()
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        ArchiveRootError,
        RootAgentError,
        SdbError,
        OSError,
        ValueError,
        RuntimeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"mirror: {error}", file=sys.stderr)
        sys.exit(1)
