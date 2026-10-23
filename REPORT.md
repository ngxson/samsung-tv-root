# TQ43Q7F3AUXXC: root result and acquisition handoff

This public report omits personal paths, network addresses, activity timestamps
and system-file fingerprints. Addresses in `192.0.2.0/24` and dump folder names
below are examples; substitute your own local values when following commands.

**TV acquisition is finished. The imported study tree
and index now exist on the Mac, and a Home-screen viewer has been live-tested.**
The USB drive was formatted with the user's explicit approval,
the dump was captured and verified, and the drive was safely unmounted from
both the TV's global mount namespace and the root session's namespace.

The acquisition handoff below records the earlier state before Mac import.
Its pending-import instructions and `usb-acquisition/` paths are historical:
the current dump has `dist/dump/rootfs/`, `index.sqlite`, `images/`, `metadata/`
and `study.json`. Their presence was observed during the screen-capture work;
the full import was not revalidated in that session. Inspect the existing
study tree before doing more acquisition or import. Do not format the drive again.
These continuation notes describe the current local workspace; `build/` and
`dist/` are ignored by Git and will not accompany a fresh clone.

## Confirmed root result

A temporary authenticated root session was confirmed on a Samsung Q7F using
the same runtime-detection workaround described in
[#5](https://github.com/chris-ritsen/samsung-tv-root/issues/5).

| Item | Value |
| --- | --- |
| Model / platform | `TQ43Q7F3AUXXC` / `25_RSL_QTV` |
| OS / kernel | Tizen `9.0.0`, Linux `5.4.261`, `armv7l` |
| Internal build | `T-RSLFDEUC-0090-REL-202608061939` |
| Active .NET runtime | `Microsoft.NETCore.App/6.0.9` |
| Host tooling | macOS ARM64; SDB `4.2.36` inside Docker |
| Project | v0.0.9, commit `bcd6b2c`, local runtime-detection fix; release payloads |

The SDB injection worked as `uid=901(sdk)`, but stock `archive-root` stopped at
`dotnet --list-runtimes`: Samsung's apphost returned `Unknown option
--list-runtimes.` with exit 255. The fallback checks that `libcoreclr.so` and
`System.Private.CoreLib.dll` are readable, then resolves:

```text
/usr/share/dotnet.tizen/netcoreapp
  -> /usr/share/dotnet/shared/Microsoft.NETCore.App/6.0.9
```

Older version directories, including `3.1.3`, are aliases to `6.0.9` on this TV.
The original capability and tar checks remain in place:

```text
/usr/bin/dotnet cap_setgid,cap_sys_admin=ei
/usr/sbin/sdbd-tarlauncher cap_setgid,cap_setuid=eip
tar: --to-command supported
```

The authenticated agent executed `id; uname -a` successfully (exit 0):

```text
uid=0(owner) gid=0(root) ... context="User"
Linux Samsung 5.4.261 ... armv7l GNU/Linux
authenticated=true; uid=euid=gid=egid=0
cap_eff=0000003fffffffff
staging_cleaned=true; system_files_unchanged=true
```

Per-run payloads and archives were removed. SHA-256 hashes of `/etc/passwd` and
`/usr/share/sdbd/public.pem` matched before and after the session. No persistent
service, firmware change, or debloating was performed.

SMACK remained `User`, so this does not establish unrestricted access across
every security boundary. Early SDB reconnect attempts failed, but later Docker
SDB sessions successfully completed the acquisition and final cleanup. An
interactive PTY was not validated; the user had asked to finish the shell
launcher without its live test. UEP disable and persistent root were not tested.

## Current state and locations

The original objective is to dump the TV into `./dist/dump` and prepare it for
offline study. Wi-Fi transfers were slow, so the user requested acquisition
directly to an attached USB, then explicitly authorized erasing that USB.

| Item | Current state |
| --- | --- |
| Workspace | Repository root (`.`) |
| TV / Mac callback | User-supplied; examples: `192.0.2.50` / `192.0.2.10` |
| SDB | Existing Docker container `tizen-sdb`; adapter `tools/sdb_docker.py` |
| USB | FAT32 USB drive; example volume label `TV_DUMP` |
| USB dump folder | `samsung-tv-dump-example` |
| Former TV USB mount | `/opt/media/USBDriveA1`, `/dev/sda1`; now unmounted |
| Canonical acquisition records on Mac | `dist/dump/usb-acquisition/` |
| Actual archive/image payloads | On the USB, **not yet imported to the Mac** |
| Existing `dist/dump/rootfs/` and `index.sqlite` | From the earlier incomplete Wi-Fi attempt; **not the completed study tree** |
| Latest device session | Finished; staging cleaned and checked system files unchanged |
| Physical step still required | User must connect the USB to the Mac |

At the acquisition handoff, the USB had not appeared on the host. The host had
sufficient free space for import and extraction.

## What was captured

The dataset contains **11,524,210,099 bytes (10.73 GiB)** of archive/image
payloads, plus metadata. There are 73 compressed raw chunks covering five
devices without gaps. The chunk hashes were checked against USB readback.

| Device | Uncompressed bytes | Chunks |
| --- | ---: | ---: |
| `/dev/mmcblk0` | 7,818,182,656 | 59 |
| `/dev/mmcblk0boot0` | 4,194,304 | 1 |
| `/dev/mmcblk0boot1` | 4,194,304 | 1 |
| `/dev/subroot1` | 1,433,403,392 | 11 |
| `/dev/subroot2` | 55,574,528 | 1 |

The subroot devices are active filesystem views overlapping the main storage;
their sizes are not additional independent physical capacity.

Seven filesystem exports are also present. Archives are split into parts of
at most 1 GiB for FAT32; raw chunks represent at most 128 MiB of source data.

| Logical archive | Stored bytes | Tar exit |
| --- | ---: | ---: |
| `archives/prd.tar.gz` | 51,566,911 | 0 |
| `archives/factory_peq.tar.gz` | 6,930,686 | 0 |
| `archives/mnt__systemrw.tar.gz` | 430,134 | 0 |
| `archives/mnt__drm.tar.gz` | 348,533 | 0 |
| `archives/opt__usr__apps__HEPsqFNie0__res__wgt.tar.gz` | 13,181,583 | 0 |
| `archives/root.tar` | 2,436,341,760 | 2 |
| `archives/opt.tar` | 3,174,031,360 | 2 |

The two exit-2 archives are valid readable exports with documented access
denials, not complete file-level exports. Their producer/readback hashes
matched during capture. They were retained, and their combined and per-part
hashes were recorded in a subsequent USB read pass. Their manifest entries
have `file_export_complete: false` and list these seven `read_errors`:

```text
/usr/apps/dpm/plugins
/zone/config
/zone/securezone
/zone/template/securedata
/opt/data/dpm
/opt/zone/securezone
/opt/zone/template/securedata
```

The raw device images retain the underlying storage bytes. Runtime sockets
were ignored by tar and recorded in the `/opt` log/manifest. The TV's tar lacks
xattr/SELinux support, so file exports do not preserve SMACK labels or other
extended attributes. Separate `metadata/file-capabilities-*.txt` inventories
are included. Raw images preserve filesystem metadata as stored on disk.

This was a live acquisition, not an atomic snapshot or tested restoration
backup. RAM, virtual filesystems and RPMB were not captured. Files were read
through `/proc/1/root` to avoid the root session's temporary `/etc/passwd` and
SDB verification-key overlays. Acquisition SDK staging was excluded from file
exports. Keep the dump private; it contains device state.

## Next steps on the Mac

1. Wait for the user to attach the USB to the Mac. Expect a folder such as
   `/Volumes/TV_DUMP/samsung-tv-dump-example`; verify the actual mount path.
2. From the repository root, run the prepared importer:

   ```sh
   python3 -u build/tv-check/import-usb.py
   ```

   This helper has been prepared but **has not yet been run against the USB**.
   It finds the matching `.acquisition-token`, requires the USB and local
   manifests to agree, checks every copied artifact's size and SHA-256, and
   uses private local permissions. It stages the import at
   `dist/dump-usb-import`, then preserves the old incomplete directory as
   `dist/dump-previous` and installs the verified import at `dist/dump`.
   It also carries over final eject evidence that exists only on the Mac.
   If a guard fails, inspect the state rather than bypassing it or deleting
   the earlier attempt. These import/backup paths did not exist at handoff.
3. Build the study tree:

   ```sh
   python3 -u tools/index_dump.py
   ```

   This verifies and joins archive parts, extracts regular files into
   `rootfs/`, builds `index.sqlite`, assembles `images/*.img`, and generates
   `partitions.json`, `study.json`, `SHA256SUMS` and a study `README.md`.
   Allow roughly 35 GiB of local space. Run on the Mac filesystem, not directly
   on FAT32: the assembled main image exceeds FAT32's file-size limit.
4. Verify that `study.json` lists five assembled images with the sizes above,
   all seven filesystem archives were indexed, and the guide still documents
   the seven protected paths. Compare the two original-file hashes with the
   private acquisition records and report the actual indexed file/binary counts.
   Do not execute acquired TV binaries on the Mac.

The indexer records Linux permissions, ownership, links, special nodes and
available PAX metadata in SQLite; it does not apply those permissions or create
links/device nodes on the host. Case/Unicode collisions are stored separately
under `collisions/`, with mappings in SQLite and `collisions.json`.

Original system-file hashes were checked again before finalization. Keep the
expected values in private acquisition records. After indexing, compare those
records with:

```sh
shasum -a 256 dist/dump/rootfs/etc/passwd dist/dump/rootfs/usr/share/sdbd/public.pem
```

## Evidence and implementation

Before import, the primary evidence paths are:

- `dist/dump/usb-acquisition/manifest.json`: canonical artifact sizes, hashes,
  coverage, timestamps, read errors and retry history.
- `dist/dump/usb-acquisition/README.md`: USB-side usage guide mirrored locally.
- `dist/dump/usb-acquisition/metadata/root-session-capture.json`: completed
  capture session evidence.
- `dist/dump/usb-acquisition/metadata/root-session.json`: completed archive
  catalog session evidence.
- `dist/dump/usb-acquisition/metadata/usb-eject-root-session.json` and
  `usb-eject-operations.json`: final checks, sync and unmount evidence.
- `dist/dump/usb-acquisition/logs/archives__root.tar.log` and
  `archives__opt.tar.log`: exact access denials and tar warnings.
- `dist/dump/usb-final-recovery.log`, `usb-retain-exports-retry.log`, and
  `usb-eject.log`: successful final phases.
- `build/tv-check/archive-root-result.json`: original root proof.

The final evidence reports authenticated UID/EUID/GID/EGID 0,
`launcher_exit_observed=true`, `staging_cleaned=true` and
`system_files_unchanged=true`. Historical failure logs remain for provenance,
including `metadata/session-error.txt`; they do not describe the final session.
There are no unresolved transfer attempts in the final manifest. Its
`resolved_by` fields mean the readable exports were retained; they do **not**
mean the seven protected paths became readable.

Relevant local changes are uncommitted:

- `shell.sh`, `tools/archive_shell.py`, `tools/sdb_docker.py`: Docker-backed
  interactive shell launcher and SDB adapter.
- `src/samsung_tv_root/archive_root.py`: runtime-path fallback, persistent
  session handler, reconnect/cleanup handling and explicit launcher-exit
  observation for long sessions. Cleanup and original-file checks remain
  required even if the launch transport times out.
- `src/samsung_tv_root/sdb.py`: persistent capture support.
- `tools/dump_tv.py`: network acquisition and inventory.
- `tools/dump_tv_usb.py`: direct USB acquisition, split archives, readback
  verification, resumability and optional uncompressed exports.
- `tools/index_dump.py`: offline verification, extraction, index and image
  assembly.
- `README.md`, `QUICKSTART.md`, associated tests, and this report.

Focused checks passed during the work: USB acquisition tests (10), archive
session/preflight tests (24 passed, 4 live tests skipped), indexer tests (8),
and network acquisition tests (4). This was not a full repository test-suite
run. Live capture, hash verification, cleanup and USB unmount all completed;
full-dataset Mac import/indexing still needs its live validation.

## Operational details if further TV work becomes necessary

No TV connection is needed for the pending import/index work. For future TV
access, use the existing Docker SDB adapter and one TV session at a time.
`./shell.sh "$TV_HOST"` uses your configured TV address and detects the callback
address. Export `TV_HOST` before running it. The payload configuration uses the
verified v0.0.9 release under
`build/tv-check/release/samsung-tv-root-0.0.9-macos-arm64/_internal/payloads/`.

Relevant lessons from the completed acquisition:

- Large TV-side `gzip` exports aborted with exit 134; the cause was not
  established. The two large exports were captured as plain tar instead.
- The TV's default awk number formatting switched to scientific notation
  above 2 GiB. The counter now formats an integer explicitly and checks a
  value above 2 GiB on the TV before acquisition.
- The released C# agent doubles backslashes through its command argument
  handling on this runtime. Use the existing tested command construction;
  literal newline escapes in awk/PID files caused earlier failures. Background
  workers now close inherited fd 3 and record PIDs with `echo`.
- Do not background an entire `mkdir && command` list while retaining the
  agent's output descriptors; it can prevent the execute response from
  completing. Create the runtime directory first, then launch the worker with
  all standard streams redirected.
- Agent `WRITEFILE` uses `FileMode.CreateNew`; staging filenames must be unique.
  Metadata publication uses unique staging and a small tar bundle.
- Abandoned copy jobs and failed catalog staging were cleaned up. The final
  successful helpers were `build/tv-check/resume-usb-clean.py`,
  `adopt-readable-exports.py`, and `finish-usb.py`. They have already run;
  **do not rerun them as the next step**. Continue with `import-usb.py` instead.

Do not commit or publish the dump, device state, payload binaries or local
evidence. The earlier public root-test summary above can be shared separately
from these local continuation notes.

## Screen viewer and remote controls

**The current viewer streams the Q7F compositor at 640x360 and 4 fps.** It
includes a direction pad, OK, Back, Home, volume up/down, mute and digits 0–9.
Export `TV_HOST` with your TV's address and start it with `./mirror.sh`; the browser
opens at `http://127.0.0.1:8765`.
Keyboard shortcuts and resolution/quality options are in `QUICKSTART.md`.
Press Ctrl-C to stop the capture process and clean up the root session.

The first proof used `enlightenment_info -dump_screen` and produced a valid
1920x1080 PNG of the Home interface. That command took about two seconds just
to capture, and the full capture/transfer cycle took 3.7–4.7 seconds. Resizing
on the Mac could not achieve the requested 4 fps.

The updated `tools/tv_mirror.py` uses the TV's installed GStreamer pipeline:

```text
waylandsrc ! video/x-raw,format=BGRA,width=640,height=360,framerate=4/1
  ! videoconvert ! video/x-raw,format=I420 ! jpegenc quality=65 ! fdsink
```

It runs once per session with `XDG_RUNTIME_DIR=/run` and
`WAYLAND_DISPLAY=wayland-0`, sending concatenated JPEGs over a separate TCP
callback. A session token and TV peer-address check authenticate the video
connection. The Mac parses complete JPEGs, shows the newest frame through an
MJPEG HTTP endpoint and measures received frame rate. It does not repeatedly
start capture commands or write full-resolution PNGs on the TV.

The web interface is loopback-only. Control POSTs require a session token and
an allowlisted button name; arbitrary shell commands are not accepted. Each
button invokes `/usr/bin/enlightenment_input_key` with the corresponding key
name. The existing script emits down and up events through the compositor's
D-Bus interface. A bounded queue keeps control traffic separate from video.
The experimental Menu button was removed because it did not visibly affect
the Home screen; the shipped viewer has 20 buttons.

Live validation:

- A 240-frame run finished at 3.995 fps; a subsequent run with navigation and
  volume controls finished at 3.998 fps after 121 frames. These are received
  frame rates measured over the last 40 frames, not the configured target.
- Frames were approximately 21–34 KiB. Changing Home UI and volume overlays
  were visible, confirming that the stream updates when the interface changes.
- Up/down/left/right, Home, Back, volume up/down, mute and digit 0 commands
  completed successfully. Typical command acknowledgements took 0.16–0.22
  seconds; Home took about 0.60 seconds in the final run. Other digits and OK
  use the same tested dispatch route and are covered by mapping tests.
- The local MJPEG endpoint returned complete JPEG frames. Requests without
  a control token, with an unexpected Host, or with an arbitrary command as
  the button name were rejected.
- Finite capture and Ctrl-C both stopped cleanly. Final evidence reports
  `staging_cleaned=true`, `system_files_unchanged=true` and
  `launcher_exit_observed=true`.

An early streaming benchmark exposed a shutdown-order bug: waiting for the
TCP server to close before closing its accepted client could hang. That test
process was stopped, its exact staging directory was removed, and original
system-file hashes were independently verified. The final implementation
closes clients first; a loopback regression test covers this case. The final
live runs exited normally. No test viewer or root session remains running.

Focused tests cover fragmented JPEG framing, embedded marker bytes, size
limits, button authorization, queue bounds, native key mapping, stream
shutdown and archive session lifecycle. Browser rendering could not be
checked through UI automation because no browser/native-app surface was
available; the HTTP endpoints were exercised and captured images inspected.
Individual app video surfaces, protected playback and audio streaming have
not been validated; audio is not included in this viewer.

Local evidence (ignored by Git):

- `build/tv-screen-probe/screen.png`, `commands.json`, `session.json`: first
  full-resolution screenshot proof.
- `build/tv-fast-mirror/commands.json`, `frames.mjpg`, `first.jpg`: native
  continuous-capture probe.
- `build/tv-fast-mirror/controls-verification.json`, `final-run.log`,
  `final-session.json`: control acknowledgements, frame-rate and cleanup checks.
- `build/tv-fast-mirror/benchmark-recovery.json`: cleanup of the early stalled
  benchmark. Older PNG-era logs under `build/tv-mirror/` are historical.
- `build/tv-mirror/latest.jpg`, `latest.json`, `last-session.json`, `stream.log`:
  the latest frame, measured rate, successful cleanup and capture diagnostics.

The source changes are `mirror.sh`, `tools/tv_mirror.py`, `tools/tv_mirror.html`,
`tests/test_tv_mirror.py` and the usage documentation. Screenshots may contain
private TV content; local evidence is not part of the shareable source.

## Samsung TV Plus disabled

**The native TV Plus disable call succeeded on this Q7F.** The reported virtual
channel count changed from **nonzero to 0**, and a subsequent read still returned
0. TV Plus autorun was already off and remains off. The TV Plus app and
deeplinker packages remain installed; no package files, firewall rules or
boot services were changed.

The app was already absent from the Samsung Home app row, which did not mean
the service was disabled. Samsung documents a separate **Disable** action
that removes the Home entry and TV Plus guide channels, with **Add to Home**
under Connected Devices as the normal way to re-enable it:
[Samsung's disable/re-enable instructions](https://www.samsung.com/au/support/tv-audio-video/disable-samsung-tv-plus/).

Inspection of the local firmware dump identified
`Tizen.TV.Broadcast.Hybridcast.TVPlus.FCTVPlus.SetTVPlusOnOff(bool)` and the
underlying Freesia D-Bus transport. A read-only `GetFreesiaVersion` call first
confirmed the transport, returning `FREESIA_PLATFORM_2025_0.4`. The disable
request used Samsung's own service with JSON `{"ret":false}`; its response
decoded to `{"ret":true}`. `db/tvplus/support` remains true because the TV still
supports TV Plus; that regional capability is separate from the channel state.

To repeat the disable operation, stop the mirror, export `TV_HOST` with your TV's
address, run `./shell.sh`, and paste this command **inside the TV root shell**:

```sh
dbus-send --system --print-reply --reply-timeout=10000 \
  --dest=com.samsung.FCCallListener /com/samsung/Object \
  com.samsung.FCCallListener.MethodCall \
  string:SetTVPlusOnOff int32:13 \
  array:byte:123,34,114,101,116,34,58,102,97,108,115,101,125
vconftool get db/mw/number_of_virtualservices
```

The corresponding enable/undo command sends `{"ret":true}`:

```sh
dbus-send --system --print-reply --reply-timeout=10000 \
  --dest=com.samsung.FCCallListener /com/samsung/Object \
  com.samsung.FCCallListener.MethodCall \
  string:SetTVPlusOnOff int32:12 \
  array:byte:123,34,114,101,116,34,58,116,114,117,101,125
```

The enable call is derived from Samsung's launcher code but was not executed
during this test, to leave the requested disabled state in place. Reboot
persistence, firmware-update behavior, and pressing the dedicated TV Plus
remote button have not been tested. This disables the service's channels;
it does not establish that Samsung Home promotional content is removed.

The diagnostic root session closed normally with `staging_cleaned=true`,
`system_files_unchanged=true` and `launcher_exit_observed=true`. Local evidence
is in `build/tvplus-disable/`: `disable.json`, `verify-final.json`,
`api-read-native.json`, `session.json`, and the disable/enable command files.
The temporary inspection helpers and screenshots there are ignored by Git.
`busctl` could send this legacy service request but failed to decode its byte
array response; use `dbus-send`, which decoded the replies successfully.

## OTA DNS block and updater logs

**The TV sees the configured DNS overrides, but no failed update check was
found in the retained logs.** On-device `getent ahosts` and `nslookup` both
resolved `osb-v2.samsungqbe.com` and `samsungotn.net` to the configured local
DNS sink address (omitted here). These checks confirm the two exact names,
not coverage of all firmware endpoints or
subdomains. No update check was triggered and no updater settings were changed.

During inspection, `SWUAutoUpgrade` was running and
`db/menu/support/softwareupdate/autoupdate` was 0. `SWUCoreTV` and `SWUService`
were inactive, with their latest runs outside the retained log window. Their
clean process exits do not establish whether an update search succeeded.

The system journal had no files, and the main/system `dlogutil` buffers were
empty. This release uses `vlog`: a filtered `vlogutil` export successfully
retrieved updater messages. The available circular log covered only about
214–241 seconds. Its updater entries were watchdog heartbeats and a Wi-Fi
policy notification; none reported DNS resolution, connection, TLS, download
or update-search failure. The earlier core/service runs were outside that
retention window. Consequently, the absence of an error here cannot confirm
or refute whether an earlier OTA request was blocked.

For future passive observation, use `vlogutil` with the
`org.tizen.tv.swu.SWU` tag or the current updater PIDs. The useful dump command
was `vlogutil -P <current-updater-pids> -v kerneltime -o <temporary-file> '*:V'`;
substitute the current comma-separated PIDs. Plain `nslookup <name> <server>`
works on this TV; its version rejects `-type=A` and `-type=AAAA`.

Local evidence is in `build/tv-ota-check/`: `initial-logs.json`,
`updater-vlog.txt`, `swu-tags.txt`, `dns-final.json`, and `session.json`.
The diagnostic root session closed with staging cleaned and the original
system-file hashes unchanged. The mirror was paused for exclusive SDB access
and restarted after inspection.
