# ESP32 Samsung Smart Remote receiver

ESP-IDF + NimBLE firmware that takes the TV's place as BLE central for the
Samsung Smart Remote (see [../REMOTE_PROTO.md](../REMOTE_PROTO.md)). It:

1. scans for the remote in pairing mode (name "Smart Control …" or Samsung
   manufacturer data `0x0075`; the 2016 remote only has the name),
2. connects and bonds the way the TV does (Just Works, no PIN), storing the
   keys in NVS so the remote reconnects on its own after a reboot,
3. dumps the full GATT table, the HID Report Map (raw and parsed) and every
   readable characteristic, which fills §9 items 1–3 of the protocol doc,
4. subscribes to every notifying characteristic and decodes key presses
   using the Report Map.

> Pairing the remote to the ESP32 **un-pairs it from the TV** (the remote
> keeps a single bond). To go back, hold Back + Play/Pause near the TV.

## Supported chips

Any ESP32 with BLE: ESP32, S3, C3, C6, H2, C2. **Not ESP32-S2** (it has no
Bluetooth). Built and tested to compile with ESP-IDF v6.1 for esp32, esp32s3
and esp32c3.

## Build and flash

```bash
. ~/work/esp-idf/export.sh
cd esp32-receiver
idf.py set-target esp32        # or esp32s3 / esp32c3 / esp32c6 ...
idf.py build flash monitor     # add -p /dev/cu.usbserial-XXXX if needed
```

## Pairing

1. Switch off nearby Samsung TVs, or the TV may grab the remote first.
2. Flash, open the monitor and wait for `Scanning: hold Back + Play/Pause…`.
3. Hold **Back + Play/Pause** on the remote for 3 s, keeping it close to the
   board.
4. You should see `Connecting…`, `Link encrypted (bonded=1…)`, the GATT dump,
   and finally `CONNECTED - press keys on the remote`.

After that the bond survives reboots. Pressing any key wakes the remote, and
it reconnects by itself.

To **forget the remote**, hold the BOOT button (GPIO0, or GPIO9 on C3/C6/H2)
for 3 s while the firmware runs. It erases the bond and reboots.

## Output

```
I (5120) srr: RAW rid=0x02 h=0x001e: e9 00
I (5120) remote: KEY DOWN VolumeUp         (Consumer:0x00e9)
I (5230) srr: RAW rid=0x02 h=0x001e: 00 00
I (5230) remote: KEY UP   VolumeUp         (Consumer:0x00e9)
I (6001) remote: VENDOR rid=0x05 h=0x0026 len=8
I (6001) remote: 01 23 45 ...
```

- `KEY DOWN/UP`: standard keys (Keyboard / Consumer / Generic Desktop pages)
  decoded from the Report Map.
- `VENDOR`: Samsung vendor-page reports, printed raw. Press each button once
  to build the key→report table.
- `AXIS`: touchpad/pointer/sensor values, if the remote sends any.
- `NOTIFY`: notifications from non-HID (vendor service) characteristics.

To act on keys (forward over UART/MQTT, drive GPIO, ...), edit
[main/remote_events.c](main/remote_events.c). Every callback runs on the
NimBLE host task, so keep them short.

## Voice

The mic is host-gated (REMOTE_PROTO.md §6): the remote only streams audio
after the host writes `31 04` to the `0xFB` Feature report while MIC is
held. **Hold MIC, speak, release.** The board runs the whole session itself:

1. On press it sends `31 04` (start), then `31 03 10` (extend), as the TV does.
2. On release it sends `31 02` (stop).

The audio arrives on the `0xF6` Input report: 172 bytes per report, 44 per
second. Each report is a sequence byte followed by three 57-byte **mSBC**
frames (16 kHz mono). The board logs each report as a `voice: FRAME` hex
line. The LED stays on while it waits for audio and blinks rapidly while
audio arrives.

With `SRR_VOICE_LOCAL` (the default), voice never reaches the TV. The TV
gets neither the MIC key nor its release nor the audio, and the board drops
the TV's own voice commands.

```bash
./monitor.sh                  # terminal 1
python3 voice_capture.py      # terminal 2: one WAV per MIC press in voice-out/
python3 voice_capture.py logs/monitor-XXXX.log --once   # decode an old log
```

`voice_capture.py` follows `logs/latest.log`, so it doesn't need the serial
port. It needs `ffmpeg` to decode mSBC. On the ESP32-C3 the console is the
native USB port only (`sdkconfig.defaults.esp32c3`), because a 115200 UART
can't keep up with the audio.

## TV relay

The board can also pose as a remote to the TV: remote → ESP32 → TV. The TV
sees a device with the remote's name, advert and full GATT table (`tv.c`).
Every service is in the remote's order, and every handle up to 0x0042 is the
same as on the remote. The boot log says
`GATT layout matches the remote's handles`. The only difference is the
`9e5d1e47…` vendor service, which sits at 0x0043 instead of 0xff00. TV writes
to the vendor characteristics are logged but not forwarded. Every input report from the remote (keys,
command replies, audio) is forwarded to the TV. Every `0xFB` Feature write from
the TV is forwarded to the remote and logged as `voice: TV->RC …`, so the log
shows exactly which voice commands the TV sends.

1. Start the TV's remote pairing search. The TV never searches on its own:
   a real remote's Back + Play sends the `XF86BTPairing` key over IR. On a
   rooted TV, run
   `app_launcher -f org.tizen.SmartRC -u 5001 type XF86BTPairing`
   instead (REMOTE_PROTO.md §2.5). Pairing from the TV's Bluetooth settings
   doesn't work.
2. While the search popup is up, short-press BOOT. The board advertises as
   "Smart Control 2016" for 60 s and the LED blinks slowly. Keep the real
   remote out of pairing mode meanwhile. The log shows
   `tv: PAIRED with TV …`.
3. From then on the board advertises whenever the TV is disconnected and
   the remote is connected, and the TV reconnects on its own. The board
   never advertises to the TV while it scans for the remote: the C3
   controller asserts (`sch_arb.c 517`) when the TV connects during a scan. Short-press BOOT again to pair a different
   TV. Holding BOOT for 3 s still erases all bonds.

Voice stays on the board even while a TV is connected (`SRR_VOICE_LOCAL`).
To let the TV run voice through the relay instead, turn that option off.

The TV only treats a device as a Smart Remote (and enables voice for it) if
it is in its Smart RC list. bt-service adds a device to that list when its
HID vendor/product is `0x04E8:0x2075` (Samsung). This remote reports Realtek
`0x005D:0x0001`. By default the relay presents the remote's PnP ID with the
product ID + 1 (`0x005D:0x0002`), so the TV can tell the two apart. If the
TV pairs the relay but never sends `31 …` commands, set `SRR_TV_PNP_ID` to
`02e80475200100`, erase the bonds and pair again.

## Options (`idf.py menuconfig` → *Samsung remote receiver*)

| Option | Default | Meaning |
|---|---|---|
| `SRR_STRICT_FILTER` | y | Pairing candidate must be Samsung + discoverable + have a HID hint (UUID `0x1812` or HID/remote appearance) |
| `SRR_MIN_RSSI` | -80 | Ignore pairing candidates weaker than this |
| `SRR_LOG_RAW` | y | Print every raw HID report |
| `SRR_DUMP_READABLE` | y | Read and print every readable characteristic after connecting (reads only) |
| `SRR_CLEAR_BOND_GPIO` | 0 / 9 | BOOT button: short press = TV pairing mode, hold 3 s = forget all bonds. `-1` to disable |
| `SRR_VOICE_KEY` | 160 (`0xA0`, MIC) | Hold-to-talk key, `0` = any key |
| `SRR_VOICE_LOCAL` | y | Voice stays on the board: the TV never gets the voice key or the audio |
| `SRR_VOICE_START_CMDS` | `3104` | Start command, sent on key press |
| `SRR_VOICE_EXTEND_CMD` | `310310` | Sent once after the start, like the TV |
| `SRR_VOICE_STOP_CMD` | `3102` | Stop command sent on key release, empty = none |
| `SRR_VOICE_PRE_CMD` | empty | Sent before the start (`34311a0000` = "no latency" on; the TV doesn't send it on MIC) |
| `SRR_VOICE_POST_CMD` | empty | Sent after the stop |
| `SRR_VOICE_LATENCY0` | n | Also set LL connection latency to 0 (the TV doesn't) |
| `SRR_VOICE_MAX_MS` | 30000 | Voice session time limit |
| `SRR_LED_GPIO` | 8 (C3) / 2 | Status LED: blink per key press, solid during a voice session, rapid blinking while audio arrives. `-1` to disable |
| `SRR_LED_ACTIVE_LOW` | y (C3) | LED polarity |
| `SRR_TV_RELAY` | y | Pose as a remote to the TV and relay both ways |
| `SRR_TV_NAME` | `Smart Control 2016` | Name shown to the TV |
| `SRR_TV_PNP_ID` | empty | PnP ID shown to the TV. Empty = the remote's with product ID + 1; `02e80475200100` = Samsung `04E8:2075` |
| `SRR_TV_PAIR_S` | 60 | Pairing mode duration |

Every Samsung advert is logged with the reason it was skipped, for example
`-> skip: strict filter: no HID UUID/appearance`. If your remote never
connects, check those lines; if it is being skipped, turn off
`SRR_STRICT_FILTER` and keep the remote close to the board.

## Known unknowns

- The voice commands (`31 04` start, `31 02` stop) come from TV firmware
  analysis (bt-service `__bt_send_voice_cmd`), not a capture.
- The firmware accepts whatever connection parameters the remote requests,
  which is what the TV does to save the remote's battery.

## Files

| File | Purpose |
|---|---|
| `main/main.c` | Scan, connect and bond, discover services, read, subscribe, dispatch notifications |
| `main/hid_parser.c` | HID report descriptor parser and input-report decoder |
| `main/hid_usages.c` | Names for common Keyboard / Consumer / Generic Desktop usages |
| `main/remote_events.c` | Application hooks (logging by default) |
| `main/voice.c` | Voice session: start command, latency, audio frame logging |
| `main/tv.c` | TV relay: GATT server copying the remote, advertising, pairing, forwarding |
| `voice_capture.py` | Saves each voice session from the log as a WAV (mSBC via ffmpeg) |
