# Quick start

## 1. Install Tizen Studio

Install Tizen Studio from Samsung/Tizen. This project does not redistribute
Samsung's `sdb` executable.

## 2. Enable Developer Mode

On the TV:

1. Open Smart Hub and Apps.
2. Focus App Settings and enter `12345`.
3. Enable Developer Mode.
4. Enter the controller computer's LAN IPv4 address.
5. Reboot the TV.

## 3. Check the release

Extract the matching release and run:

```console
./samsung-tv-root doctor
```

The examples below use `./samsung-tv-root` on Linux and macOS. On Windows, run
the same arguments with `samsung-tv-root.exe`. Release archives contain the TV
payloads; they do not require a local payload build. Put an unusual Tizen Studio
location before the command, for example
`./samsung-tv-root --sdb /path/to/sdb doctor`.

These instructions describe v0.0.9 and the current source tree. `archive-root`
requires v0.0.9 or newer, or a source checkout. For v0.0.8 and earlier downloads,
follow the bundled documentation or the
[v0.0.8 README](https://github.com/chris-ritsen/samsung-tv-root/blob/v0.0.8/README.md).

To run from source instead of a standalone release:

```console
git clone https://github.com/chris-ritsen/samsung-tv-root.git
cd samsung-tv-root
python -m venv .venv
.venv/bin/python -m pip install -e .
.venv/bin/python -m samsung_tv_root doctor
```

On Windows, use `py -3.12` and `.venv\Scripts\python.exe`. When running from a
source checkout, preflight needs only Python and `sdb`; root commands also
require GNU Make, the .NET 6 SDK, and built TV payloads. Run
`make common-payload qn90b-payload qn90f-payload` for the model-specific routes.
The archive route needs OpenSSL on the host's `PATH`; for a TV with .NET 3.1,
also run `make common31-payload` with SDK 6.0.428 installed. That build restores
the .NET 3.1 reference packages from NuGet. See the
[source instructions](README.md#run-from-source).

For source commands below, replace `./samsung-tv-root` with
`.venv/bin/python -m samsung_tv_root` (Windows:
`.venv\Scripts\python.exe -m samsung_tv_root`).

## 4. Save the TV profile

Create the configuration, then edit the file path printed by `config init`:

```console
./samsung-tv-root config init
./samsung-tv-root config check
```

The default file is `%APPDATA%\samsung-tv-root\config.toml` on Windows,
`~/Library/Application Support/samsung-tv-root/config.toml` on macOS, and
`$XDG_CONFIG_HOME/samsung-tv-root/config.toml` on Linux when
`XDG_CONFIG_HOME` is set, otherwise `~/.config/samsung-tv-root/config.toml`.

The generated `televisions.my-tv` table is a named profile. With one configured
QN90F profile, the model-specific commands infer it. Use `--profile NAME` when
more than one configured TV has the same model. A nonstandard Tizen Studio
installation can be saved as a top-level `sdb = "C:/path/to/sdb.exe"` value.

## 5. Verify and root the TV

Using the saved profile:

```console
./samsung-tv-root preflight qn90f
./samsung-tv-root qn90f root
```

On Windows, the exact commands are:

```bat
samsung-tv-root.exe preflight qn90f
samsung-tv-root.exe qn90f root
```

To run a local Bash script without installing it or replacing the TV's Bash:

```bat
samsung-tv-root.exe qn90f root --script httptoolkit.sh
```

The script is staged under volatile `/run` storage and removed with the root
session. Use `--script-argument VALUE` repeatedly when arguments are needed.

An explicit address remains supported:

```console
./samsung-tv-root preflight qn90f 192.0.2.50
./samsung-tv-root qn90f root 192.0.2.50
```

Use `qn90b` for the 2022 PontusM platform. Preflight reports `tested`,
`compatible-untested`, or `incompatible`. The QN90F route verifies restoration
of its temporary credential and GPU page-table changes before accepting root.
The QN90B route changes its own exploit process's credentials and exits after
the requested command; it does not perform the QN90F restoration sequence.

The archive route in v0.0.9 and newer has no model whitelist. It checks the SDB
foothold, required file capabilities, tar support, and installed .NET runtime,
then stages and executes a root payload. `probe` performs the full root
acquisition and cleanup; it is not a read-only preflight:

```console
./samsung-tv-root archive-root probe 192.0.2.50
./samsung-tv-root archive-root root 192.0.2.50 --command 'id'
```

This route was live-tested on QN90B and QN90F. It does not yet provide an
interactive shell, disable UEP, or replace the controller's root method. It
cleans its per-run TV staging files after a normal session.

All root routes here require a working SDB connection. If TCP port 26101 refuses
connections, resolve Developer Mode and SDB availability before running root
commands. A reachable TV network API does not prove SDB is available. If Samsung's
apphost rejects `dotnet --list-runtimes`, archive-root resolves the active
`/usr/share/dotnet.tizen/netcoreapp` runtime and verifies its core libraries
before selecting a supported payload.

### Interactive shell with the local Docker setup

This checkout includes a launcher for the existing `tizen-sdb` Docker container:

```sh
./shell.sh 192.0.2.50
```

Replace `192.0.2.50` with your TV's address; all `192.0.2.x` addresses here are
documentation examples. You can also use `export TV_HOST=192.0.2.50` (with
your TV's address) to omit the address from the shell, mirror and dump commands.
There is no default TV address.
The launcher detects the controller's LAN address and uses the local payload
build or the v0.0.9 release downloaded under `build/tv-check/release`.
Docker must be running. The TV must have Developer Mode
enabled for the controller's address and provide Samsung's signed
`iot-hub-control/bin/socat` relay.

The launcher obtains temporary root through the archive route and opens a
persistent Bash PTY. Commands such as `cd` and `export` retain their state;
Ctrl-C interrupts the remote command. Type `exit` or press Ctrl-D to close the
shell. The temporary listener accepts only the controller's IP; closing the
shell removes session staging and disconnects SDB. Do not share the TV's SDB
connection with another agent during a session.

To override the TV address or controller address:

```sh
./shell.sh 192.0.2.50 --callback-host 192.0.2.10
./shell.sh --help
```

### View this TV's menus on the Mac

With the same Docker and payload setup:

```sh
./mirror.sh 192.0.2.50
```

This opens a viewer at `http://127.0.0.1:8765`, streaming the TV's compositor at
**640x360 with a target of 4 fps**. Continuous capture replaces the slower
full-resolution PNG screenshots. Live tests on this Q7F held approximately
4.00 fps, including changing Home screens and volume overlays; JPEG frames
were about 21–34 KiB. Individual app video surfaces and protected playback
have not been validated. Audio is not streamed.

The viewer includes direction buttons, OK, Back, Home, volume up/down, mute
and a 0–9 number pad. Keyboard shortcuts: arrows, Enter, Esc/Backspace, Home,
digits, `+`/`-` for volume and `M` for mute. Each click sends a complete key
press/release; number keys behave according to the active TV app or input.

Press Ctrl-C in the terminal to stop the TV capture process, close the root
session and remove its temporary files. Keep the SDB connection exclusive to
this tool while it runs.

```sh
./mirror.sh 192.0.2.50 --callback-host 192.0.2.10
./mirror.sh 192.0.2.50 --width 640 --height 360 --fps 4 --quality 65
./mirror.sh 192.0.2.50 --no-open --frames 1    # save one JPEG and exit
./mirror.sh --help
```

On shutdown, the latest frame and measured frame rate are saved to
`build/tv-mirror/latest.jpg` and `latest.json`; successful session cleanup is
recorded in `last-session.json`. The live frame is also available at
`http://127.0.0.1:8765/frame.jpg`. The old `latest.png`, if present, is an earlier
full-resolution capture.

The web viewer and controls listen only on the Mac's loopback interface. A
separate temporary video callback on the Mac's LAN address accepts this TV
with a per-session token. The implementation uses the TV's existing GStreamer
Wayland capture and JPEG encoder, plus `enlightenment_input_key` for controls.
No new TV service or native payload is installed.

### Dump this TV for offline study

With the same Docker and payload setup, run from the repository root:

```sh
PYTHONPATH=src:tools python3 tools/dump_tv.py --host 192.0.2.50 --phase all
python3 tools/index_dump.py
```

The local acquisition script requires `--host` or `TV_HOST` and writes to
`dist/dump` by default. It exports the original mounted filesystems and reads the main eMMC, boot
regions and active subroot devices into resumable chunks. Compression defaults
to the TV to reduce network traffic; use `--compression host` on a faster link
to move that work to the Mac. SHA-256 verifies each source stream and records
each stored artifact.
Rerunning the command reuses completed files after checking their hashes.

The indexer produces `rootfs/`, `index.sqlite`, assembled raw images, a partition
map and `dist/dump/README.md`. Linux links and special nodes are recorded rather
than created on the Mac. Read the acquisition logs for missing metadata or
read errors. Writable filesystems are captured live; RAM, virtual filesystems
and RPMB are outside this dump. Keep the private device data local; `dist/` is
ignored by Git.

For a USB drive mounted on this TV at `/opt/media/USBDriveA1`, capture directly
to a dedicated folder instead:

```sh
PYTHONPATH=src:tools python3 tools/dump_tv_usb.py --host 192.0.2.50 --directory samsung-tv-dump-example
```

This uses an existing FAT32/exFAT USB filesystem with at least 15 GiB free.
Large filesystem archives are split into 1 GiB parts; raw images use 128 MiB
source chunks. Each completed artifact is hashed during capture and read back
from USB for verification. Local progress and metadata are under
`dist/dump/usb-acquisition/`. After copying the USB dump folder to the Mac,
`tools/index_dump.py` verifies and joins the parts before preparing the study
tree. Keep the USB attached until the capture reports that writes are synchronized.
If the TV aborts `gzip` during a large filesystem export, rerun the USB command
with `--uncompressed-files`; completed exports are reused and missing ones are
written as plain tar parts.

## 6. Keep root available

Run the controller with the same configuration:

```console
./samsung-tv-root daemon run
```

On Linux, the optional host service is:

```console
./samsung-tv-root service install --enable --now
./samsung-tv-root service status
```

The host controller reacquires volatile root after TV lifecycle events. It does
not install persistence on the TV. Remote observation and remapping remain off
until devices and rules are explicitly configured. Native TV event collection
also remains off until `events.enabled = true` is set for that TV.

Further end-user details are in the [controller](docs/user/CONTROLLER.md) and
[remote input](docs/user/REMOTE_POLICY.md) guides. Implementation and reversing
records are listed separately in the [documentation index](docs/README.md).
