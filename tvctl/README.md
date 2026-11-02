# Samsung TV persistent root channel ("rootkit")

A long-lived, authenticated root backdoor for a Samsung Tizen TV. One exploit
run installs a TV-side listener; every later session connects directly over
TCP — **no SDB, no Tizen Studio, no Developer-Mode dance, no exploit**.

Live-validated on 2026-11-02 against `TQ43Q7F3AUXXC` (QN90F-class, Tizen 9.0,
armv7l, .NET runtime 6.0.9) at `192.168.2.174`.

## TL;DR — daily use

```sh
# after a TV power cycle or 'rootkit stop' (takes ~15 s, runs the exploit once):
python3 tvctl/tvctl.py activate 192.168.2.174

# any time the TV is on — direct TCP, no SDB:
python3 tvctl/tvctl.py status                 # PING the listener
python3 tvctl/tvctl.py exec id                # one command as root
python3 tvctl/tvctl.py exec df -h /
python3 tvctl/tvctl.py shell                  # interactive root bash (pty)
python3 tvctl/tvctl.py stop                    # authenticated shutdown
python3 tvctl/tvctl.py uninstall               # stop + remove TV-side files
```

State (TV host, port, shared secret) lives in `rootkit/state.json` (chmod 600).
**Lose it → re-run `activate`.** The TV is unchanged otherwise.

## What it is

```
┌──────────── Mac (host) ────────────┐      ┌──────── TV ────────────────┐
│ tvctl.py activate                │      │                            │
│   └─ archive-root exploit (SDB) ───┼─────►│ /usr/bin/dotnet Backdoor-  │
│        one time, ~15 s             │      │   Agent.dll --listen 23101  │
│                                    │      │   (uid 0, detached, no     │
│ tvctl.py exec / shell / status   │      │    stdout, survives the     │
│   └─ direct TCP 23101 ─────────────┼─────►│    exploit session)         │
└────────────────────────────────────┘      └────────────────────────────┘
```

- **TV side** (`payload/BackdoorAgent.cs`): a fork of the repo's
  `SamsungTvRootAgent.cs` with a new `--listen PORT SECRET_FILE` mode. Runs
  under Samsung's signed `/usr/bin/dotnet` (so no UEP bypass needed), detached
  via `setsid()` with stdio on `/dev/null`, root uid/gid. Serves the existing
  authenticated EXEC / READFILE / WRITEFILE / SHUTDOWN protocol plus
  `BACKDOOR_STOP` (authenticated process exit) over a TCP listener that
  accepts sessions serially and never exits on client errors.
- **Host side** (`tvctl.py`): reuses `samsung_tv_root`'s
  `RootAgentConnection` client unchanged. Only the handshake differs.

## Wire protocol

Same frame format as the repo's root agent: tab-separated payloads, each line
followed by an HMAC-SHA256 over `nonce ‖ 0x00 ‖ payload` with the shared
secret (32 random bytes, generated at activation, stored base64 in
`state.json` on the Mac only).

Backdoor handshake (listener side):

```
TV  →  SAMSUNG-TV-ROOT/1\t<32-byte hex challenge>\n
Mac →  PROOF\t<hmac_sha256(secret, challenge ‖ 0x00 ‖ "PROOF") hex>\n
TV  →  READY\t<hmac>\n            (nonce = 32 zero bytes from here on)
     … normal root-agent frames: PING/EXEC/READFILE/WRITEFILE/SHUTDOWN …
```

The secret **never crosses the wire** in either direction; a wrong secret
gets the connection dropped after the PROOF frame. This is
proof-of-possession, not encryption — traffic is plaintext on the LAN.

## Files

| Path | Purpose |
|---|---|
| `rootkit/payload/BackdoorAgent.cs` | TV-side listener (modified `SamsungTvRootAgent.cs`, see diff below) |
| `rootkit/payload/BackdoorAgent.csproj` | net6.0 build (TV runtime is 6.0.9) |
| `rootkit/payload/out/SamsungTvBackdoorAgent.dll` | built payload (22 KB, arch-neutral IL) |
| `tvctl/tvctl.py` | host CLI: activate / status / exec / shell / stop / uninstall |
| `rootkit/state.json` | host, port, secret — **secret, keep private** |
| `rootkit/activation-evidence.json` | exploit evidence from the last clean activation |

## Build (only if you change the C#)

No dotnet SDK on the Mac — build in Docker:

```sh
docker run --rm -v "$PWD/rootkit/payload:/src" -w /src \
  mcr.microsoft.com/dotnet/sdk:6.0 \
  bash -c "rm -rf out obj && dotnet build BackdoorAgent.csproj -c Release -o out"
```

Architecture-independent IL; runs on the TV's armv7 net6.0 runtime via
Samsung's signed `dotnet` host.

## Prerequisites (host)

- Docker with a `tizen-sdb` container running `sdb` (`tools/sdb_docker.py`
  wraps it; TV's sdbd port 26101 must be reachable from the container —
  use host networking for the container if connect fails).
- The v0.0.9 release payloads for the archive-root exploit
  (`build/tv-check/release/samsung-tv-root-0.0.9-macos-arm64/_internal/payloads/common`,
  auto-detected) or `make common-payload`.
- Developer Mode enabled on the TV (per the repo README) — only needed for
  `activate`, never for daily use.

## How activation works (`activate`)

1. Sets `SDB=tools/sdb_docker.py` (Docker SDB), ensures the release payloads.
2. Runs the repo's `archive_root.probe()` — the SDK archive-launcher root
   route (`SVE-2025-50109`-class sdb package-name injection → signed
   tar launcher → `dotnet` `cap_setgid`/`cap_sys_admin` → uid 0).
3. Inside the temporary root session (EXEC as root):
   - `pkill -f '[S]amsungTvBackdoorAgent.dll --listen'` (reap stale
     listeners from earlier activations),
   - stage `SamsungTvBackdoorAgent.dll`, its `.runtimeconfig.json`, and a
     fresh `backdoor.secret` into
     `/home/owner/share/tmp/sdk_tools/samsung-tv-root-kit/`
     (base64 through EXEC, because the callback agent's WRITEFILE is
     sandboxed to its own staging dir),
   - launch `/usr/bin/dotnet …/SamsungTvBackdoorAgent.dll --listen 23101 …/backdoor.secret >/dev/null 2>&1 &`
   - **verify**: connect over TCP, complete the PROOF handshake, PING.
   - state.json is written **at this point**, inside the session.
4. Exploit cleanup (staging removal, system-file hash check) runs afterwards.

### Known quirk: `activate` may not exit by itself

After the backdoor is verified and state saved, the exploit's cleanup can
hang on a wedged sdbd session; its persistent capture thread then blocks
asyncio's default executor at interpreter shutdown, and the process appears
"stuck" even though everything already succeeded. The code handles this
(`asyncio.wait_for(probe(...), 180)` + `os._exit(0)` when the backdoor was
verified). If a run ever looks stuck: **check `rootkit status` first** — it
almost certainly already succeeded.

### After a TV reboot

The listener is volatile by design (no boot persistence, nothing modified in
system files — the exploit verifies `/etc/passwd` and sdbd's public key are
unchanged). Power cycle → `activate` (~15 s) → back. If the TV is slow to
boot, the first `activate` may fail the shell-injection timing check; wait a
minute and retry.

## Security properties / caveats

- Secret only in `state.json` on the Mac and `backdoor.secret` (0644, sdk
  share) on the TV. Both ends need it; the wire carries only HMACs.
- No encryption, no source-IP filter — anyone on the LAN can *reach* the
   port but cannot authenticate without the secret. If the TV is on a
  hostile LAN, treat the channel accordingly.
- Commands run as full root via `/bin/sh`; the file-transfer commands are
  limited to the install dir + per-agent runtime dir under `/run`.
- `BACKDOOR_STOP` is authenticated like every other frame; `uninstall`
  removes the TV-side directory (root needed → runs the exploit once more).

## BackdoorAgent.cs changes vs upstream SamsungTvRootAgent.cs

- `--listen PORT SECRET_FILE [STATUS_FILE]` mode: `TcpListener` on 0.0.0.0
  (SO_REUSEADDR), serial accept loop, up to 4 concurrent client sessions,
  per-session thread, never exits on client error.
- PROOF handshake before the command loop (challenge-response HMAC).
- Command loop (`RunCommandLoop`) factored out of callback-mode `RunSession`;
  both modes share EXEC/READFILE/WRITEFILE/SHUTDOWN handling.
- `BACKDOOR_STOP` frame → `BYE` + `Environment.Exit(0)` (listen mode only,
  guarded by the `SAMSUNG_TV_BACKDOOR=1` env var set in listen mode).
- `IsAllowedFilePath` additionally allows the install directory and
  tolerates a null staging dir (listen mode).
- Callback mode (`BackdoorAgent.dll HOST PORT TOKEN STAGING`) still works —
  it is the original agent behavior.
