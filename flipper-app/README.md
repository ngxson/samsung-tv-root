# Dummy USB microphone for Flipper Zero

Launch **Apps → USB → Dummy USB Mic** to expose a USB Audio Class 1.0 microphone
and a CDC ACM serial debug port. The microphone generates quiet pseudo-random
background noise: **16 kHz, signed 16-bit little-endian, mono**, 32 bytes per USB
millisecond. It does not sample a physical microphone. The default peak level is
128 out of 32768 (about −53 dBFS RMS).

This is an experiment to find out whether the TV enumerates and opens an external
USB microphone. Enumeration alone does not establish that Samsung voice services
use it. TV behavior is not yet verified.

## Display and buttons

- **HOST READING**: audio IN transfers have completed within the last 250 ms.
- **Open, waiting for reads**: the host selected the streaming alternate setting,
  but no recent audio packets completed.
- **Idle**: USB is configured but the host has not enabled the audio stream.
- **Read total**: cumulative audio milliseconds transferred since launch/reset.
- **Session**: audio milliseconds transferred since the last stream start/reset;
  retained when the stream stops.
- **OK** resets counters; **Back** exits and restores normal Flipper USB.

Durations count completed 16-sample packets, not time since opening the app.
The initial empty hardware buffer is excluded. This establishes USB host activity,
not which process reads the audio or whether it stores, analyzes, or uploads it.
Counters are in RAM, persist across cable changes/reconnects, and wrap after about
49.7 days of continuous transfers. Silence mode still counts transfers.

## Build and install

Install [uFBT](https://github.com/flipperdevices/flipperzero-ufbt) and use an SDK
matching the device's firmware API. The connected development Flipper uses
Unleashed `unlshd-089e`, API **87.8**; the `unlshd-089` SDK matches it:

```sh
cd flipper-app
export UFBT_HOME="$PWD/.ufbt"
ufbt update --url https://github.com/DarkFlippers/unleashed-firmware/releases/download/unlshd-089/flipper-z-f7-sdk-unlshd-089.zip
./build.sh
python -m pip install pyserial
python tools/deploy.py --no-build
```

For official or other firmware, initialize the appropriate SDK instead. This is
an external `.fap` app; no firmware flash is required. The output is
`dist/dummy_mic.fap`, installed at `/ext/apps/USB/dummy_mic.fap`. You can also copy
it using qFlipper, close qFlipper, then open it on the device.

`tools/deploy.py` builds, sends `exit` to an earlier instance, uploads, launches,
then verifies a response on the new debug port. It does not close unrelated apps.
Its default `dev` launch argument restores normal USB and exits after 15 seconds
if enumeration never succeeds. Menu launches remain open indefinitely; pass
`--normal-launch` to deploy with the same behavior. The watchdog cannot recover a
firmware crash; Back remains the fallback if USB is unavailable.

Plain `./build.sh launch` also uploads and starts the app, but uFBT may report
`Device not configured` when the original serial port disappears. Check the new
port with `micctl.py ping` to establish whether launch actually succeeded.

## USB commands

The app has its own newline-delimited serial protocol, separate from the normal
Flipper CLI/RPC. Each command returns one `OK ...` or `ERR ...` line. CR, LF, and
CRLF work; lines are limited to 63 ASCII characters. Baud rate is nominally
115200 8N1. Commands execute in the app thread, outside USB interrupt callbacks.

```sh
python tools/micctl.py ping
python tools/micctl.py status
python tools/micctl.py --watch 1
python tools/micctl.py level 128
python tools/micctl.py silence
python tools/micctl.py noise
python tools/micctl.py reset
python tools/micctl.py reconnect
python tools/micctl.py exit
```

| Command | Effect |
| --- | --- |
| `help` | List commands |
| `ping` | App/version response |
| `status` | JSON with configuration, alternate setting, recent reads, total/session milliseconds, packet/start/stop/error/overflow counters, signal mode and level |
| `reset` | Clear counters without stopping the stream |
| `noise` / `silence` | Select generated noise or digital zero |
| `level N` | Noise peak amplitude, 0–4096; leaves silence mode unchanged |
| `reconnect` | Re-enumerate the composite device; retains counters and level |
| `exit` | Acknowledge, exit the app, restore the original Flipper serial interface |

Port discovery uses the app's USB identity and serial string. Override with
`--port /dev/ttyACM0` or `--port COM5`. Close other serial clients during commands.
Exit/reconnect responses are flushed with a bounded wait so an unread debug port
cannot hold the app open forever.

The local prototype uses `1209:0001`, an experimental USB identity, and serial
`dummy_mic_001`. It is not a unique production allocation; connect one instance at
a time and use an allocated VID/PID before distributing a hardware product.

## TV experiment

1. Launch the app, then connect Flipper to the TV with a USB data cable.
2. In the existing TV root shell, inspect `dmesg`, `cat /proc/asound/cards`, and
   `cat /proc/asound/pcm`. Check `ls /dev/ttyACM*` for the debug interface.
3. Leave it idle and watch the display. Then exercise the TV feature you suspect
   might open a microphone; compare the state and counters. A manual recording
   proves the driver works but does not prove the TV opens it automatically.
4. If ALSA tools are present, `arecord -l` identifies the capture card. A manual
   five-second probe is `arecord -D hw:CARD,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/dummy-mic.wav`
   (substitute the actual card). The TV may lack audio/CDC drivers or userland tools.

The Flipper can have only **one USB host** at a time. When it is plugged into the
TV, the computer cannot directly address its serial port. Send commands through
the TV root shell, using the discovered device path:

```sh
stty -F /dev/ttyACM0 115200 raw -echo
printf 'exit\n' > /dev/ttyACM0
```

For responses, keep the TTY open with a reader (for example `cat /dev/ttyACM0` in
a second shell) while writing `status\n`. If the TV has no CDC driver, the audio
function may still work; USB debugging will require moving Flipper back to the
computer, or exiting with Back.

## Validation

The initial 2026-10-24 run used the connected Unleashed API 87.8 Flipper and macOS:

- Build/import checks and all 24 native C tests pass.
- Both microphone and CDC serial interfaces enumerate.
- Captured noise measured 72.64 RMS and peak 127 at level 128; silence was all
  zeros. No host audio overflow or device transfer errors were reported.
- Read counters advanced during capture and remained unchanged after stopping.
- Level changes, fragmented commands, and recovery from an overlong line worked.
- `reconnect` preserved counters; `exit` restored the normal Flipper CLI, which
  reported that no app was running. Exit also worked during active capture.

The user subsequently reported that this Mac test activated their AirPods.
Although the capture stream explicitly selected Flipper and the measured samples
matched its generated signal, that does not establish that other inputs were
untouched. PortAudio initializes device information when imported, and its
[macOS stream setup](https://github.com/PortAudio/portaudio/blob/master/src/hostapi/coreaudio/pa_mac_core.c)
enables input before binding the requested device. That is a possible explanation,
not a confirmed diagnosis of the AirPods behavior.

`check_audio.py` now refuses on macOS **before importing the audio backend or
accessing USB**. A regression test checks that ordering without recording. USB-only
commands such as `micctl.py status`, `reset`, and `exit` remain available on macOS.
TV behavior remains unverified.

For a Mac recording test, use the separate native helper instead of PortAudio:

```sh
mkdir -p dist
clang -std=c11 -Wall -Wextra -Werror tools/macos_capture.c \
  -framework CoreAudio -framework CoreFoundation -o dist/macos_capture
./dist/macos_capture 20
python tools/micctl.py status
```

It requires exactly one USB device named `Flipper Dummy Mic`, checks that its
CoreAudio UID contains `dummy_mic_001`, and registers an input callback directly
on that device. It never selects or changes the default audio device, creates an
AudioUnit, or loads PortAudio. Unexpected devices/formats cause an error instead
of fallback. The duration is 1–60 seconds; it prints sample count, RMS, and peak
without saving audio. It leaves the app running and does not reset its counters.
Launching the app again resets RAM counters, which explains the zero displayed
after the earlier validation/relaunch cycle.

The direct CoreAudio retry on 2026-10-24 captured 320,000 samples in 20 seconds
(RMS 73.51, peak 128). USB status showed `reading=true` during capture and
`reading=false` afterward, retaining a total of 20.051 seconds with zero transfer
errors. The small extra USB duration includes host buffering around the capture.

```sh
python -m pip install pytest
python -m pytest tests -q
./build.sh
# On a non-macOS host, with the app running:
python -m pip install sounddevice numpy
python tools/check_audio.py
python tools/micctl.py exit
```

The native tests compile the actual C parser/generator and inspect the actual
descriptor array. On supported hosts, `check_audio.py` requires a unique input
named **Flipper Dummy Mic**, records the selected device in its report, and checks
noise amplitude, silence, active reading, frozen counters after stop, and error
counters. It restores noise mode afterward and does not save recordings. The
audio library still enumerates other devices; selection alone is not a guarantee
that its initialization has no effects on them.

Implementation references: [USB Audio 1.0 specification](https://www.usb.org/sites/default/files/audio10.pdf),
[Flipper USB interface API](https://developer.flipper.net/flipperzero/doxygen/furi__hal__usb_8h_source.html),
and the [STM32WB USB driver](https://github.com/flipperdevices/libusb_stm32/blob/master/src/usbd_stm32wb55_devfs.c).
