# Samsung Smart Remote — Protocol Reference (TV / host ⇄ remote)

Purpose: self-contained protocol documentation so that an agent can implement a
host (e.g. a computer or an ESP32) that talks to the Samsung Smart Remote, or
instrument the TV side. Raw reverse-engineering evidence (firmware addresses,
analysis notes) lives in `REMOTE.md` at the repo root; this file contains
protocol facts and the state of the work.

Confidence markers used throughout:

- ✅ **CONFIRMED** — observed live, or official documentation
- ◐ **INFERRED** — strongly supported by firmware behavior, not packet-verified yet
- ❓ **UNKNOWN** — still open; listed in §9

Hardware context: the remote is a Realtek RTL8763B-family BLE SoC (dual-mode
BT, on-chip codec, digital mic, USB-C charging). The TV (or a replacement host)
is always the **BLE central**; the remote is the **peripheral**.

Paths in this file: `esp32-receiver/…` is next to this file; `tools/…`,
`tvctl/…`, `payloads/…`, `shell.sh` and `REMOTE.md` are at the repo root.

---

## LATEST PROGRESS (2026-11-02)

**Status** (ESP32-C3 running `esp32-receiver/`, remote "Smart Control 2016",
firmware `EA` / `592200`):

| Area | Status |
|---|---|
| Pair and bond with the remote (Just Works), automatic reconnection | ✅ |
| Key presses (`0xF7` report, §5.4), release checksum verified | ✅ Power `0xD2`, Down `0x61` and MIC `0xA0` named; the other codes are not yet |
| **Voice: hold MIC → audio on the computer** | ✅ one WAV per press, audio confirmed correct by ear |
| Relay: the board poses as the remote to the TV, and the TV accepts its keys | ✅ |
| Voice kept off the TV (`SRR_VOICE_LOCAL`, the default) | ✅ the TV never sees the MIC key or the audio |
| Voice relayed into the TV's own capture API | ❌ 0 bytes (§6.6); not needed for the current goal |
| Next: send the voice to Home Assistant | not started |

**Findings of the last session:**

1. **The audio is on report `0xF6`, encoded as mSBC** (§6.4). Each 172-byte
   notification holds a sequence byte and three 57-byte mSBC frames: 16 kHz
   mono, 44.4 reports/s. The earlier assumption (IMA-ADPCM on `0xFB`) was
   wrong, which is why every earlier attempt "got no audio": the audio was
   there all along, on the report nobody decoded.
2. **The voice session** (§6.3) is `31 04` while a key is held, then
   `31 03 10` once, then `31 02` on release. The TV sends exactly this. The
   `34 31 1A/1B` latency commands are not needed.
3. **The relay's TV side** was blocked by a NimBLE bug in our firmware: UUID
   pointers to stack memory. With it fixed, the TV completes its HID setup
   (§6.6). The TV caches the GATT table per BD address, so the board's
   address must change after any GATT layout change.

**How to use it now** (from `esp32-receiver/`, see its README):

```bash
./monitor.sh                 # terminal 1: board log → logs/latest.log
python3 voice_capture.py     # terminal 2: hold MIC, speak, release → voice-out/voice-<time>.wav
```

Current board state: own address `0c:4e:a0:61:b8:0d` (`SRR_OWN_ADDR`),
bonded to the remote and to the TV. To pair the remote again, hold
Back + Play/Pause near the board.

**To pair the TV with the board**, run this on the TV to open its remote
pairing search, then short-press BOOT on the board:
`app_launcher -f org.tizen.SmartRC -u 5001 type XF86BTPairing`
(or send the IR pairing code, §2.5).

**Next steps:**
- **Home Assistant:** `voice_capture.py` already produces the decoded 16 kHz
  mono PCM; send it to HA instead of writing a WAV (e.g. the Wyoming
  protocol).
- **Key names:** name the remaining key codes with `esp32-receiver/keymap.sh`.
- **Open protocol items:** see §9.

---

## 1. Actors and transports

| Link | Transport | Notes |
|---|---|---|
| Pairing + keys + battery + all control | BLE, HID-over-GATT (HOGP) + Samsung vendor reports | Standard HID service plus vendor report IDs |
| Voice (mic audio) | BLE HID report `0xF6` notifications (same HID service) | mSBC, 16 kHz mono (§6.4) |
| Firmware update (OTA) | BLE GATT (Realtek DFU service) or HID SET_REPORT | §7 |
| Device management on TV | D-Bus + vconf keys on the TV | §8 |

---

## 2. Pairing protocol

### 2.1 Entering pairing mode (remote side) — ✅ CONFIRMED

1. Press and hold **Back + Play/Pause** simultaneously for ≥ 3 seconds
   (all remote buttons: `⬅` + `▶⏸`).
2. The remote's LED blinks → it is advertising and will accept a connection from
   any central.

### 2.2 Connection and bonding — ✅ CONFIRMED (headless pairing)

- The remote in pairing mode is a **BLE peripheral**; the host scans and
  **initiates** the connection (the remote does not connect out).
- Bonding is **"headless"** — Just Works, no PIN/passkey dialog.
- The remote stores exactly one bond (its central partner). Practical
  consequences:
  - **Pairing the remote to a new host (e.g. an ESP32) un-pairs it from the TV.**
  - Re-pairing to the TV later is always possible: hold Back+Play/Pause and let
    the TV connect.
- After bonding, the remote reconnects to its bonded central with **directed
  advertising** when a key is pressed. An unbonded host only sees it while it
  is in pairing mode.

### 2.3 Post-connect handshake (what the TV does) — ✅ seen live through the relay

On a successful connect, the TV (central):

1. Exchanges MTU (247), pairs/encrypts, enables Service Changed indications.
2. Discovers the GATT table and reads PnP ID, HID Info, the Report Map and
   **every Report Reference descriptor**, then enables notifications on the
   `0xF7`, `0xF6` and `0xFB` Input reports, and exchanges MTU again.
3. Sends `34 31 07 00 00`, then the device info query `31 01 17` (answered by
   `33 17 …`), then `31 24 10` (§6.2).
4. Stores remote identity/state (battery, firmware version, device type,
   capability, name) in its configuration keys (§8.1).
5. Accepts the remote's power-saving connection parameters (observed request:
   interval 20 ms, **peripheral latency 99**, timeout 8.1 s).

On reconnection it reads nothing from GATT: it reuses everything cached at
pairing (§6.6).

### 2.4 Recommended host-side pairing recipe — ✅ (ESP32)

1. Power off nearby Samsung TVs (or move away) — the TV will grab the remote
   within seconds otherwise.
2. Put the remote in pairing mode (Back + Play/Pause 3 s).
3. Scan; connect to the advertising remote. Observed pairing-mode advert
   ("Smart Control 2016", public address `38:8c:ef:…`):
   - ADV_IND (connectable, legacy PDU — no extended advertising):
     flags `0x05` (LE Limited Discoverable, BR/EDR not supported),
     appearance `0x0180` (Generic Remote Control),
     complete 16-bit UUIDs `1812` (HID), `180F` (Battery), `180A` (Device Info),
     peripheral connection interval range AD present.
     Raw: `02 01 05 05 12 10 00 20 00 03 19 80 01 07 03 12 18 0f 18 0a 18`
   - Scan response: complete local name `"Smart Control 2016"`.
   - **No manufacturer-specific data** (in particular no Samsung `0x0075`).
4. Bond Just Works (IO capability NoInputNoOutput, initiated by the central),
   then MTU 247, then discover. Use an ESP32 or Linux: on macOS the HID
   service (`0x1812`) is hidden from apps (§6.5).
5. If the OS claims it as an HID keyboard, standard keys will start
   controlling the machine — that is expected (§5.2).

### 2.5 How the TV searches for new remotes — ✅ CONFIRMED (TV firmware)

**The TV never scans for remotes spontaneously.** Its pairing search is
launched by a key event, exactly like launching an app:

1. **`XF86BTPairing` key** (X11 keycode 153, Dali EXTENSION_KEY `0xA0`).
   On a real remote, holding **Back + Play/Pause** sends this key over
   **IR** — the Smart Control remotes are dual IR+BT, and the pairing combo
   is an IR signal, not a BLE one.
2. The enlightenment **keyrouter** rule
   (`usr/share/enlightenment/data/config/tizen-tv/module.keyrouter.cfg`)
   force-launches **`org.tizen.SmartRC`** with extra data `type:XF86BTPairing`
   (rule applies only when SmartRC is not already running).
3. `org.tizen.SmartRC` logs "XF86BTPairing key pressed..Launch SmartRC
   Pairing..Set UI Mode == TRUE", starts a discovery popup
   (`SMART_CONTROL_POPUP_STATE_DISCOVERY_START`), connects Just Works to
   the first suitable advert, then queries device info (`31 01 17`, §6.2).

**Consequence for a replacement peripheral (ESP32 relay):** putting the board
in pairing mode does nothing on its own — **the TV must be told to search.**
Options:

| Trigger | How | Notes |
|---|---|---|
| **Rooted TV** | `app_launcher -f org.tizen.SmartRC -u 5001 type XF86BTPairing` | uid 5001 = owner (same pattern as the TV's own `appstore.sh`, `art_start.sh`) |
| IR ✅ (used live) | replay the real remote's Back + Play IR signal: **Samsung32, address `0x07`, command `0xD1`** | captured with a Flipper Zero (file `Sam.ir`, signal `Sams_D1`); the search popup opens and the TV connects to the board within ~1 s |
| Settings menu | TV Settings → … remote/accessory pairing | menu path not verified ❓; pairing from the TV's Bluetooth settings does not work |

The IR pairing code, as saved by the Flipper Zero (`/ext/infrared/Sam.ir`):

```
Filetype: IR signals file
Version: 1
#
name: Sams_D1
type: parsed
protocol: Samsung32
address: 07 00 00 00
command: D1 00 00 00
```

Samsung32 sends the address twice and the command followed by its
complement, so the frame bytes are `07 07 D1 2E` (LSB first on the air;
`0xE0E08B74` in the bit-reversed LIRC-style notation).

After bonding, the TV stores the "remote" (its Smart RC list, vconf keys
§8.1) and reconnects automatically whenever the board advertises (~1.5 s).

---

## 3. GATT table — ✅ CONFIRMED

Dumped from a "Smart Control 2016" remote (fw type `EA`, version `592200`) by
the ESP32 receiver, after bonding. Props: R read, W write, w
write-without-response, N notify, I indicate.

| Service | Handles | Characteristic (value handle, props) | Notes |
|---|---|---|---|
| GATT `0x1801` | 0x0001–0x0004 | `0x2A05` Service Changed (0x0003, I) | |
| GAP `0x1800` | 0x0005–0x000d | `0x2A00` name (0x0007, R) = "Smart Control 2016"; `0x2A01` appearance (0x0009) = `0x0180`; `0x2A04` (0x000b) = `10 00 10 00 63 00 2a 03`; `0x2AA6` (0x000d) = empty | |
| Device Info `0x180A` | 0x000e–0x0020 | `0x2A29` "Realtek BT", `0x2A24` "Model Nbr 0.9", `0x2A25` "RTKBeeSerialNum", `0x2A27` "RTKBeeHardwareRev", `0x2A26` "RTKBeeFirmwareRev", `0x2A28` "RTKBeeSoftwareRev", `0x2A23` = `00 01 02 00 00 03 04 05`, `0x2A2A` "RTKBeeIEEEDatalist", `0x2A50` PnP ID = `01 5d 00 01 00 03 00` | Realtek SDK placeholder strings, not real versions |
| Battery `0x180F` | 0x0021–0x0024 | `0x2A19` (0x0023, RN) | Read returned an empty value at connect |
| HID `0x1812` | 0x0025–0x003c | `0x2A4A` HID Info (0x0027) = `11 01 00 00`; `0x2A4C` Control Point (0x0029, w); `0x2A4E` Protocol Mode (0x002b, Rw); `0x2A4B` Report Map (0x002d, 285 bytes); Report `0x2A4D` ×4 at 0x002f, 0x0033, 0x0037, 0x003a | See §4 for the report IDs |
| Vendor `0000d1ff-3c17-d293-8e48-14fe2e4da212` | 0x003d–0x0042 | `0xA001` (0x003f, wN); `0xA002` (0x0042, Ww) | Sends one all-zero 16-byte notification on subscribe. Not the voice channel (§6.5) |
| Vendor `9e5d1e47-5c13-43a0-8635-82ad38a1386f` | 0xff00–0xffff | `e3dd50bf-f7a7-4e99-838e-570a086c666b` (0xff02, WNI); `92e86c7a-d961-4091-b74f-2409e72efe36` (0xff05, Ww) | Probably Realtek DFU/OTA ❓ |

The **HID Report Map** (`0x2A4B`) is reproduced in §4 and is the authoritative
report catalog. It declares more report IDs than there are Report
characteristics; only the four in §4.0 exist on this remote.

---

## 4. Report catalog

All application traffic rides HID reports on the Report characteristics:
input reports arrive as notifications (remote → host), host → remote commands go
out as **Write Report / SET_REPORT**.

### 4.0 Report characteristics — ✅ CONFIRMED (Report Reference descriptors, live)

| Value handle | Report ID | Type | Use |
|---|---|---|---|
| 0x002f | `0xF7` | Input (notify) | **Samsung key report**, §5.4. Every button arrives here |
| 0x0033 | `0xF6` | Input (notify) | **Voice audio**, §6.4 |
| 0x0037 | `0xFB` | **Feature** (R W w, no notify; reads fail with ATT error `0xC3`) | **Command channel**, §6.2 |
| 0x003a | `0xFB` | Input (notify) | Command replies (e.g. Dev Info `33 …`, §6.2). No audio |

Over GATT the value carries **no report-ID byte**, in both directions: the
report ID lives in the Report Reference descriptor. Key notifications on
`0x002f` are 6 bytes (the report payload), and commands are written to
`0x0037` without a leading `FB` (§6.2).

### 4.1 Report Map (decoded) — ✅

| Report ID | HID page / usage | Payload | Direction | Purpose | Conf. |
|---|---|---|---|---|---|
| `0x01` | Keyboard (0x07): modifier byte + 6-key array | 8 B | Input | Standard keys (numbers, arrows modifiers) | ◐ no traffic seen |
| `0x02` | Mouse (0x01): 3 buttons + relative X, Y (int8) | 5 B | Input | Touchpad pointer motion | ◐ no traffic seen |
| `0x07` | Vendor 0xFF01 / usage 0x14 | 20 B | Input | Vendor input (touch/sensor family) | ◐ |
| `0x08` | Vendor 0xFF01 / usage 0x14 | 20 B | Output | Vendor output (touch/haptic/LED config family) | ◐ |
| `0xF6` | Consumer (0x0C), array-style | 180 B | Input | **Voice audio**: 172-byte notifications, mSBC (§6.4). Declared as a Consumer array, but it carries no key events | ✅ live |
| `0xF7` | Vendor 0xFF00 / usage 0x2F | 6 B | Input | **Samsung key report** (§5.4) | ✅ |
| `0xF8` | Vendor 0xFF00 / usage 0x2F | 6 B | Input | Vendor short telemetry | ◐ |
| `0xF9` | Vendor 0xFF00 / usage 0x2F | 18 B | Input | Dev Info (battery %, firmware version, device type ID, capability, language version) | ◐ |
| `0xFB` | Vendor | 192 B + 11 bits | Input | Command replies (sub-ID in byte 0: Dev Info `33`, `60` ❓) | ✅ live |
| `0xFB` | Vendor (Report Reference: Feature) | 2–5 B | **Feature (host → remote)** | **Command channel** — device info, voice session, latency (§6.2) | ✅ |
| `0x20` | *(not in Report Map; accepted via SET_REPORT)* | `20 <len> <data…>` | Output/Feature | **MBR command** (low-power/latency control); remote answers with an MBR_RETURN_REPORT and the TV exposes it via callback | ✅ fw |
| `0x23` | *(not in Report Map; accepted via SET_REPORT)* | `23 <data…>` ≤ 8 B | Output/Feature | **Haptic pattern** (haptic-capable remote types only) | ✅ fw |

Notes:
- Remote **hardware type ID** values seen in firmware: `0x20, 0xE1, 0xE2, 0xE4,
  0xE6, 0xE7, 0xE9, 0xEA, 0x05` (one per firmware variant).
- The TV logs `Ignore report Id = 0x%02x received` for report IDs it did not
  register.
- Sensor / auxiliary data structures delivered by TV callbacks (from TV .NET
  binding): `SmartRcSensorData {sampling_time, sensor_type, touch_mode, x, y, z}`;
  grip-sensor state (ON/OFF/N.A.); battery-charging status events (2 words).

---

## 5. Key press protocol

### 5.1 Vendor behavior on the TV — ✅ CONFIRMED (effects)

- Key release reports are suppressed during voice sessions (TV-side behavior).
- Some keys are "TV-native": the TV executes them itself rather than injecting
  them as input events (e.g. Power, certain hotkeys).
- The remote supports a **number pad** mode toggled from the TV
  (OSD number-pad show/hide), sent via vendor reports.
- Voice-key *press* is forwarded to TV apps as key code **462**
  (`KEY_VOICECOMMAND`).

### 5.2 On a standard HID host — ◐

- Every button arrives on the vendor report `0xF7` (§5.4), which an OS HID
  stack ignores. A host must read the raw report (ESP32, or Linux with the
  bluez `hog` plugin disabled).
- **macOS:** CoreBluetooth hides the HID service (§6.5); IOHID-level APIs
  (`IOHIDManagerRegisterInputReportCallback`) may see input reports, but
  host → remote writes (voice start, haptic, MBR) do not reliably reach the
  remote's GATT characteristics.

### 5.3 Host → remote commands — ✅ partially decoded

The host sends control via Write Report / SET_REPORT (see §4.1): MBR `0x20`,
haptic `0x23`, and the `0xFB` command channel (device info, voice, latency —
§6.2). A low-power **MBR mode** can be enabled/disabled adapter-wide (TV CAPI
`bt_adapter_set_lpm_mbr_mode`). ❓ Exact byte encodings of the remaining
commands (number-pad show/hide, voice-recognition mode) still need a capture.

### 5.4 Samsung key report (`0xF7`) — ✅ CONFIRMED (live)

Six bytes, notified on Report handle 0x002f:

```
press / auto-repeat:  CC 00 00 00 00 00
release:              00 00 00 01 00 SS
```

- `CC` = key code. Named so far: `0xD2` Power, `0x61` Down, **`0xA0` MIC**.
  Other codes seen, not yet named: `0x02`, `0x07`, `0x0b`, `0x10`, `0x4f`,
  `0x58`, `0x60`, `0x62`, `0x65`, `0x68`, `0x6a`, `0x79`, `0xb4`, `0xb9`,
  `0xf3`, `0xf4`, `0xfa` (name them with `esp32-receiver/keymap.sh`).
- The press report is **resent every ~100–120 ms while the button is held**;
  a quick tap usually produces 2 press reports. **MIC is the exception: one
  press report, no auto-repeat**, then the release when it is let go.
- Release has byte 3 = `0x01` and byte 5 = `SS`, the **8-bit sum of the `CC`
  byte of every press report sent since the key went down**, i.e.
  `SS = (CC × n) mod 256` for `n` press reports. The host can use it to detect
  lost notifications. Example for Power: 1 report → `d2`, 2 → `a4`, 3 → `76`,
  5 → `1a`, 15 → `4e`.
- Two buttons held together: the second key's press arrives without a release
  of the first, and the release checksum no longer matches the per-key sum.
  The multi-key encoding is not decoded yet ❓.

---

## 6. Voice / audio protocol

### 6.1 The mic is host-gated — ✅ CONFIRMED (live)

**Holding MIC alone produces only the MIC key report** (§5.4); the microphone
stays closed until the host writes the start command `31 04` (§6.2). The
remote's firmware only accepts the start inside a window opened by a key press
(REMOTE.md §9.6: the window closes when all keys are released, or after 1 s).
Measured live (2026-11-02, `31 04` retried every 300 ms by the TV-side
recorder):

- `31 04` for 23 s with no key held: **no audio**.
- `31 04` landing while **Down** (`0x61`) was held also started the stream,
  so any held key seems to open the window, not only MIC (one observation, ◐).
- Once started, the stream keeps running after the key is released for as
  long as the host keeps resending `31 04`; `31 02` stops it.
- Sent by the board on MIC down: the first audio report arrives ~180 ms
  after `31 04`.

This is the same host-gated design as Apple's Siri Remote and Android TV
remotes.

### 6.1.1 Voice transport generations ✅

Samsung has shipped **two incompatible voice transports**. Both are triggered
by the same TV-side API (`bt_hid_rc_start_sending_voice`):

| Generation | Remotes | Audio path | Reachable from a computer? |
|---|---|---|---|
| **A — classic-BT voice** | older mic-capable Smart Control (TV device types 58/65/66, pre-solar era) | **SCO/eSCO link**, PCM routed through the TV's kernel `btpcm` module (`/dev/btpcm0`) | ❌ Only the TV can set up eSCO for an arbitrary HID device |
| **B — HID-report voice** | 2021+ remotes on the Realtek Y25 firmware (incl. this "Smart Control 2016", fw type `EA`) | **HID Input report `0xF6`**, mSBC (§6.4); commands on `0xFB` Feature | ✅ via raw GATT (ESP32, Linux). **Not macOS** |

How to identify generation B: the HID service (`0x1812`) has a Report
characteristic whose Report Reference says **`0xFB` / Feature** (command
channel). The TV reports this remote as device type 49, yet it runs the
Realtek Y25 firmware and is generation B.

### 6.2 Command channel — framing and known commands — ✅

All commands go to the **Feature report `0xFB`** (the Report characteristic
whose Report Reference descriptor says `0xFB / Feature`, HID report type 3;
handle 0x0037 on this remote), as write-with-response.

**Framing on the wire: the GATT value does NOT include the report ID.** The
TV builds `FB <payload>` and hands it to its HID stack as a Feature
SET_REPORT (logged as `Feature Report [FB3104]`); the HID layer puts the
report ID in the descriptor, not the value. Live: `31 01 17` was answered,
`FB 31 01 17` was ignored. Below, "bytes written" is the GATT value.

```
<sub-ID> | <command> | [parameters…]
```

**Sub-ID** (byte 0) selects the handler in the remote's dispatcher:

| Sub-ID | Family | Conf. |
|---|---|---|
| `0x31` | device info query + voice session control | ✅ |
| `0x34` | link/feature control; **byte 1 must be `0x31`** or the write is dropped. Commands (byte 2): `04,05,06,07,1A,1B,20,21,22,30,31` | ✅ (list) / ◐ (roles) |
| `0x20`, `0x22` | MBR / low-power control | ✅ fw |

**`0x31` commands** (byte 1):

| Bytes written | Meaning | Conf. |
|---|---|---|
| `31 01 17` | **Device info query** (TV: `__bt_service_fetch_hid_device_info`, sent after connect). The remote requires byte 2 = `0x17` and answers on the `0xFB` Input report with sub-ID `0x33`: `33 17 00 03 00 00 02 00 02 31 a3 00 00 00 ea 59 22 00 11` (firmware type `EA`, version `59 22 00`, matching `SC_BT_Y25_EA592200`) | ✅ live |
| `31 04` | **Start sending voice** (`__bt_send_voice_cmd(4)`). Accepted only inside the key window (§6.1); otherwise silently dropped. Audio follows on `0xF6` | ✅ live |
| `31 03 <byte>` | Extend (restarts the stream timer while streaming). The TV sends `31 03 10` ~250 ms after `31 04` | ✅ live |
| `31 02` | **Stop** (`__bt_send_voice_cmd(2)`); the TV sends it on MIC release | ✅ live |
| `31 06` | Stereo voice start on the TV side; **not handled by this remote** | ✅ fw |
| `31 24 <byte>` | Parameterized command, plausibly voice-recognition mode. The TV sends `31 24 10` after connect | ◐ seen live |

**`0x34` commands** (same `34 31 <cmd> 00 00` shape):

| Bytes written | Meaning | Conf. |
|---|---|---|
| `34 31 07 00 00` | Sent by the TV first thing after (re)connect; meaning ❓ | ◐ seen live |
| `34 31 1A 00 00` | **No-latency on** (TV `__bt_set_ble_conn_no_latency(TRUE)`) | ✅ fw; not sent by the TV around voice sessions |
| `34 31 1B 00 00` | **No-latency off** | ✅ fw; not sent by the TV around voice sessions |

Behavioral notes for implementers:

- The remote **silently drops** anything outside the accepted set: wrong
  sub-ID, `34` without `31` in byte 1, `31 01` without `17`, or `31 04`
  outside the key window. Every write is still ATT-acknowledged, so "write
  succeeded" does not mean "command accepted".
- Replies seen on the `0xFB` Input report: `33 …` for `31 01 17`, and
  `60 03 00 00 00` (meaning ❓) during repeated `31 04` retries.
- The TV re-sends start as the *first* command after the remote wakes
  ("Sending start voice command from SC wake up Always must be first"), and
  tracks a pending flag and a stop timer around each send.

### 6.3 Session flow — ✅ CONFIRMED (live, hold to talk)

This is what the TV sends (seen through the relay) and what the ESP32 board
now sends on its own:

```
MIC down  → remote: 0xF7  A0 00 00 00 00 00   (no auto-repeat; opens the key window)
host      → 31 04                              (at once, while MIC is held)
remote    → 0xF6 audio reports, first one ~180 ms after the start, then 44.4/s
host      → 31 03 10                           (once, ~250–500 ms after the start)
MIC up    → remote: 0xF7  00 00 00 01 00 A0
host      → 31 02                              (stop; a report may still arrive after it)
```

No `34 31 1A/1B` latency commands are needed: the TV doesn't send them, and
the stream runs at full rate with the remote's latency-99 link.

TV-side session state (strings in the TV daemon, useful for a robust host):
`is_pressing` / `is_voice_sending` flags, "voice key press already sent once"
(dedupe), "voice key press was not sent earlier, discard voice key release",
"Voice key release, but streaming is coming!" (frames still arrive after the
stop: keep draining), and a `voice_exception_timer` safety net.

### 6.4 Audio frames (report `0xF6` Input, mSBC) — ✅ CONFIRMED (live, decoded)

Every `0xF6` notification is **172 bytes**:

```
offset 0      sequence number, +1 per report (mod 256)
offset 1..57  mSBC frame 1   (starts with the sync header AD 00 00)
offset 58..114  mSBC frame 2 (AD 00 00 …)
offset 115..171 mSBC frame 3 (AD 00 00 …)

e.g. 00 ad 00 00 a1 96 55 45 44 7e ed b6 …   (first report of a session)
```

- **Codec:** mSBC (the Bluetooth wideband-speech SBC profile): 16 kHz, mono,
  120 samples per 57-byte frame. One report = 360 samples = 22.5 ms, so
  44.4 reports/s; measured 44.5/s.
- **Losses:** use the sequence byte to detect lost reports. None were lost
  in the 3 decoded sessions (142, 148 and 197 reports).
- **Decode:** drop byte 0, concatenate the rest, and decode as mSBC, e.g.
  `ffmpeg -f sbc -i in.msbc -ar 16000 out.wav`. The result is 16 kHz s16le,
  correct speech (confirmed by ear).
- **Tool:** `esp32-receiver/voice_capture.py` does this from the board log and
  writes one WAV per session.
- The Report Map declares `0xF6` as a 180-byte Consumer array. Decoding it as
  keys produces nonsense "key presses" (which is how it was first misread).
- The TV side exposes other audio paths (stereo voice, far-field voice, with
  or without noise); their layout on the link is ❓.

### 6.5 Capturing on a non-Samsung host — platform guide

#### macOS: **blocked** ❌

**macOS cannot reach the voice command channel at all.**

- **CoreBluetooth does not expose the HID service (`0x1812`) to third-party
  apps**, whether the remote is bonded or not. Only non-HID services show
  (Device Information, Battery, the `0xA001` vendor service). This was
  verified on the same device class (Apple Siri Remote).
- macOS's kernel HID stack owns the connection and the CCCDs; IOHID
  `SetReport` calls do not map onto the remote's GATT Report characteristics.
- **The `0xA001/0xA002` vendor service is not the voice channel.** Its write
  handler expects ≥16-byte structured payloads (sync/OTA family).
- bleak on macOS uses CoreBluetooth → same wall.

#### ESP32: recommended ✅ (`esp32-receiver/`)

Nothing claims the HID device, and NimBLE gives complete raw GATT client
access. Works on ESP32 and ESP32-C3/S3. The usual "nothing happens" causes, in
order:

1. **Pair + bond first — HID writes require an encrypted link.** IO
   capability `NoInputNoOutput` (Just Works), initiate pairing **as central**
   right after connecting, store the bond (the remote keeps exactly one).
2. **Request MTU 247.** The audio reports are 172 bytes; with the default
   MTU of 23 they cannot be notified.
3. **Write to the *Feature* `0xFB` Report characteristic, without a report-ID
   byte, while a key is held**: `31 04` (then `31 03 10`; `31 02` on
   release). **Subscribe to the `0xF6` Input report: the audio comes there.**
   Writing to the *Input* `0xFB` characteristic or to `0xA002` does nothing.

The `esp32-receiver/` firmware (ESP-IDF + NimBLE) does all of this: it scans
for the advert in §2.4, bonds, negotiates MTU 247, dumps the GATT table and
Report Map, subscribes to every notifying characteristic, decodes `0xF7` keys,
and runs the §6.3 session while MIC is held. It logs each `0xF6` report as a
`voice: FRAME` hex line, which `voice_capture.py` turns into WAV files.

Extra notes:

- The remote runs with peripheral latency 99 (it may skip ~2 s of connection
  events while idle). It answers immediately after a key press, which is when
  voice commands are sent anyway. A remote that reconnects on a key press
  may notify that key before the host finished GATT setup; the firmware
  buffers those notifications.
- The ESP32-C3 controller reports the remote's directed adverts with
  address `00:00:00:00:00:00`; connect to the bonded identity address
  instead.
- On the ESP32-C3, use the native USB console: a 115200 UART cannot carry
  44 hex-logged reports per second.

#### Linux ✅ (expected to work, not tested here)

Stop the BlueZ HOG plugin from claiming the device, then own the raw GATT
table:

```bash
# 1) prevent bluez from claiming the HID device (pick one):
#    /etc/bluetooth/main.conf  →  [General] DisablePlugins=input,hog
#    (or run bluetoothd manually: sudo /usr/lib/bluetooth/bluetoothd -n -d --noplugin=input,hog)
sudo systemctl restart bluetooth

# 2) put the remote in pairing mode: hold Back + Play/Pause ≥ 3 s (LED blinks)
#    (this drops the TV bond — the remote keeps ONE bond at a time)
bluetoothctl
  [bluetoothctl] scan on
  [bluetoothctl] agent on
  [bluetoothctl] default-agent    # NoInputNoOutput / JustWorks
  [bluetoothctl] pair <MAC>
  [bluetoothctl] connect <MAC>

# 3) with a raw GATT client (btgatt-client, or bleak), read each Report
#    Reference descriptor (0x2908) and find:
#      - (0xFB, type 3 Feature) → command channel
#      - (0xF6, type 1 Input)   → audio (enable notifications)
#    Check the channel first: write 31 01 17 to the Feature char; a 33 …
#    reply on the (0xFB, Input) char confirms it.

# 4) hold MIC, write 31 04 to the Feature char, write 31 02 on release.
```

Expected: `0xF6` notifications of 172 bytes at 44.4 per second (§6.4).

#### Rooted Samsung TV: the TV-side voice trigger

The TV's Bluetooth CAPI can start a remote-mic session **without the voice
assistant** (STT/Bixby is never involved).

**TV-side commands go through `./shell.sh` (repo root)**, the lab's private
channel to the TV. Rules for agents:

- **Never look inside `shell.sh` or the tooling it calls**, never modify or
  commit it. Treat it as a black box.
- **If it misbehaves, do not debug it — ask the human.**

For non-interactive use, the same channel provides one-shot commands and
file transfer: `python3 tvctl/tvctl.py exec "<cmd>"`, `… pull <remote>
<local>`, `… push <local> <remote>` (re-establish with `… activate <tv-ip>`
after a TV power cycle). Wrap long commands in `timeout` and pull results as
files; the interactive shell is serial and single-session.

**Recording the remote mic on the TV:**

```sh
./tools/record_remote_mic.sh 15        # 15 s; hold MIC on the remote during it
# → tmp/remote_mic.pcm + tmp/remote_mic-chunks.log on this machine
```

It stages the payload (`payloads/common/SamsungTvMicCapture.cs`) at
`/run/samsung-tv-root/mic/` on the TV (tmpfs — stage again after a TV power
cycle), runs it and pulls the PCM plus a per-chunk timing log. The payload:

1. calls `bt_initialize` → `bt_audio_initialize` → `bt_hid_host_initialize`
   → `bt_hid_set_audio_data_receive_cb` (all return 0; a
   `bt_hid_host_initialize -> -22` when bt-service already owns the HID host
   is harmless);
2. **retries `bt_hid_rc_start_sending_voice` every 300 ms** for the whole
   run, because the remote drops the start unless a key is held (§6.1);
3. writes the PCM (16 kHz mono s16le) and the timing log.

Result so far: the TV sends `31 04` (seen at the relay) and the remote
streams, but the payload received **0 bytes** in every run (§6.6) ❓. To see
what the TV sends or refuses, capture `vlogutil` during the run: bt-service
logs go to **vlog, not dlog** (`Sending Voice command: 0x%2x`,
`Feature Report [%s], result = %d`, `hid not ready`).

### 6.6 ESP32 relay: remote → board → TV — ✅ keys / ❌ TV-side voice

The board is the central of the real remote and, at the same time, a
peripheral that poses as the remote to the TV (`esp32-receiver/main/tv.c`).
It was built to observe the TV's real command sequence.

**Design:**

- **GATT server** copying the remote's table handle for handle (§3): `1801`
  (only `2A05`), `1800` (name, appearance, PPCP, empty `2AA6`), `180A` with all
  nine values, `180F`, `1812` (HID Info, Control Point, Protocol Mode, the
  285-byte Report Map, the four Report characteristics with their Report
  References; Input reports are R/W/N as on the remote), `d1ff…`, `9e5d1e47…`.
  Every handle up to `0x0042` matches; `9e5d1e47…` starts at `0x0043`
  instead of `0xff00` (NimBLE assigns handles sequentially). The board checks
  its handles and UUIDs at boot.
- **Advert:** the remote's pairing-mode advert byte for byte (§2.4), flags
  `0x05` while pairing and `0x04` for reconnection. The board advertises to
  the TV only while the remote is connected.
- **PnP ID:** the remote's own with product ID + 1 (`005D:0002`) works;
  `SRR_TV_PNP_ID` can override it.
- **Pairing with the TV:** start the TV's search (§2.5), then short-press
  BOOT on the board (pairing mode for 60 s). BOOT held 3 s erases all bonds.
- **Relay:** every input report goes to the TV, except (with
  `SRR_VOICE_LOCAL`, the default) the MIC key, its release and the `0xF6`
  audio. `0xFB` Feature writes from the TV go to the remote; with
  `SRR_VOICE_LOCAL` the TV's voice commands (`31 02`…`31 06`) are dropped.

**Confirmed live:** the TV pairs, completes its HID setup (§2.3), accepts the
relayed keys, and its `31 01 17` / `33 …` exchange with the remote goes
through the relay. With `SRR_VOICE_LOCAL` off, the TV's `31 04` started the
remote's stream through the relay.

**Open:** the TV-side recorder (§6.5) got 0 bytes even while the remote was
streaming and the board relayed `0xF6` to the TV ❓. During that run the board
also dropped the TV link once (`reason=0x222`, LL response timeout), probably
because it was relaying and hex-logging 44 reports per second at the same
time ◐.

**Lessons from bringing up the TV side** (each one cost a debugging round):

1. **NimBLE UUID lifetime bug (the root cause of "TV ignores the keys").**
   `BLE_UUID16_DECLARE()` used in a *runtime* assignment points to a compound
   literal on the stack. NimBLE stores that pointer at registration (host
   sync, after the init function returned), so every Find Information / Read
   By Type response carried garbage UUIDs (format `0x02`, 16 garbage bytes).
   The TV then had no Report References, built an empty report list
   (`bta_hh_le_write_rpt: no matching report`) and looped on
   `hid not ready` (error `-29`) every 2 s. Symptom on the board: the TV reads
   PnP ID, HID Info and the Report Map, exchanges MTU, then nothing. Fix:
   static `ble_uuid16_t` objects (`BLE_UUID16_INIT`).
2. **The TV caches the GATT table per peer address**
   (`/opt/usr/apps/bluetooth-config/data/gatt_cache_<mac>`) and reads nothing
   on reconnection. After any change to the board's GATT layout, give the
   board a new BD address (`SRR_OWN_ADDR`, set with
   `esp_iface_mac_addr_set(…, ESP_MAC_BT)` before `nimble_port_init()`;
   `ble_hs_id_set_pub()` alone leaves the radio on the burned-in address) and
   pair again. The board never sends Service Changed.
3. **Not the cause** (tested): PnP ID (`005D:0002` vs Samsung `04E8:2075`),
   service order, MTU timing, Service Changed. The TV does match the device
   **name** (`interop_mtk_match_name: Device Smart Control 2016 is a match`),
   so keep the remote's exact name.
4. `gatt_get_link_encrypt_status status=0x8d` in the TV's btsnoop is
   `GATT_ENCRYPED_NO_MITM`: normal for Just Works, not an error.

**ESP32-C3 controller pitfalls:**

- `BLE assert sch_arb.c 517` + interrupt watchdog reset when a central (the
  TV) connects to the board's advert while the board scans (active or
  passive, any duty cycle). Workaround: advertise to the TV only while the
  remote is connected, i.e. while the board doesn't scan.
- Extended advertising reports `BLE_GAP_EVENT_ADV_COMPLETE` with reason 0
  just before `BLE_GAP_EVENT_CONNECT`: only treat `BLE_HS_ETIMEOUT` as the
  end of pairing mode.
- `LE Add Device To Resolving List` fails (`0x12`) for the TV's restored IRK;
  harmless, the TV uses its public address.
- Once, the remote's bond disappeared from the store after the TV paired,
  with no delete logged; not reproduced since. The firmware lists the bonds
  after each TV pairing.

---

## 7. Firmware update (OTA) — ✅ CONFIRMED flow (firmware)

Two host→remote update transports exist:

### 7.1 Newer remotes: GATT OTA (Realtek DFU)

1. Host holds a candidate firmware image and decides an update is needed
   (compares remote's reported fw version/hw type with the candidate — remote
   hardware type ID must match the image).
2. Host sets connection latency to 0, waits for ACK.
3. Host connects as GATT client to the remote's DFU service, performs **MTU
   exchange**, then streams the image in chunks with progress tracking and a
   failure timeout (on timeout: revert latency, abort).
4. Remote validates the image (image header magic + internal signature
   verification; secure boot is present) and reboots into it; the remote
   disconnects and reconnects on new firmware.
5. Host reads the new fw version/hw type via the Dev Info report.

Firmware images are Intel HEX; the host-side policy file is verified with
**RSA-2048/SHA-256** (per-image `.sign` + public key PEM) before pushing.

### 7.2 Older remotes: HID transport update

Same flow but image data goes through **HID SET_REPORT** vendor reports instead
of GATT.

### 7.3 Where to get images

The remote firmware ships inside Samsung TV firmware packages (see `REMOTE.md`
§3.3) — ten hardware-type variants per firmware drop; matching is by the remote's
type ID (§4 Dev Info report).

---

## 8. Host-side (TV) integration surface

For an agent working on a rooted TV — these are the supported touch points:

### 8.1 Configuration keys (vconf)

```
file/bluetooth/is_smart_rc_paired            # bool, pairing state
file/bluetooth/bt_smart_rc_connected         # via memory/bluetooth/... (live state)
memory/bluetooth/bt_smart_rc_connected       # live connection state
file/bluetooth/bt_smart_rc_battery           # last reported battery %
file/bluetooth/bt_smart_rc_version           # remote firmware version
file/bluetooth/bt_smart_rc_device_type       # remote hardware type
file/bluetooth/bt_smart_rc_capability        # capability flags
file/bluetooth/bt_smart_rc_language_version
file/bluetooth/bt_last_connected_smart_rc    # remote BD address (string)
file/bluetooth/bt_last_connected_smartrc_name
file/bluetooth/bt_headless_info_smart_rc1    # bonded remote slots (2 supported)
file/bluetooth/bt_headless_info_smart_rc2
memory/bluetooth/latency_status              # latency-set state machine
db/btapp/lpm_mbr                             # LPM MBR mode state
rc_fw_upgrade                                # staged remote firmware path (OTA trigger)
```

### 8.2 Daemons and interfaces

- `bt-service` (D-Bus: `org.projectx.bt`, `org.projectx.bt_product`) — owns the
  BLE connection, HID report registration, voice, latency control, OTA push.
  Its logs go to **vlog** (`vlogutil`), not dlog.
- `bt.updatemanager` (D-Bus: `com.samsung.tv.bt.updatemanager`, methods `HELLO`,
  `CHIP`, `SCFW`, `ARK`, `ARK_USB`, `EXIT`) — runs the remote-chip/SmartRC/Ark
  update state machines. `SCFW` triggers the Smart Remote firmware update.
- Files it reads: `/opt/data/ci_fw_update/smartrc_fw/*.{hex,sign}` and
  `/usr/apps/org.tizen.btapp/shared/res/fwdir/SC_BT_Public_key.pem`.
- Per-peer GATT cache: `/opt/usr/apps/bluetooth-config/data/gatt_cache_<mac>`.

---

## 9. Open items

| # | Item | How to get it | Status |
|---|---|---|---|
| 1 | Full GATT table, report ID mapping, Report Map | ESP32 dump | ✅ §3, §4 |
| 2 | Voice: start condition, report, codec, session commands | ESP32 + relay | ✅ §6.1–§6.4 |
| 3 | Key code ↔ button names | `esp32-receiver/keymap.sh` | ◐ Power, Down, MIC named (§5.4) |
| 4 | Multi-key encoding on `0xF7` | Hold two keys, compare reports | ❓ |
| 5 | Meaning of `31 24 xx`, `34 31 07 00 00`, the `60 03 00 00 00` reply, the 11 flag bits of the `0xFB` Input report | Firmware (REMOTE.md) or more live captures | ❓ |
| 6 | Does any held key (not only MIC) open the voice window? | Hold another key, send `31 04` | ◐ one observation (§6.1) |
| 7 | Stereo / far-field voice layouts | A dual-mic remote | ❓ |
| 8 | TV-side voice capture gets 0 bytes (relay or direct pairing) | `vlogutil` during `record_remote_mic.sh`; retry with the remote paired directly to the TV | ❓ |
| 9 | Relay drops the TV link while streaming (`0x222`) | Retry with frame logging reduced | ◐ board load suspected |
| 10 | IR scancode of the TV's "BT pairing" key | Flipper capture | ✅ Samsung32 `0x07` / `0xD1` (§2.5) |
| 11 | DFU/OTA service + opcodes (`9e5d1e47…`? `0xA001/0xA002`?) | Sniff an OTA session | ❓ |
| 12 | D-Bus method catalog of `org.projectx.bt` | `busctl tree org.projectx.bt` on the rooted TV | ❓ |
| 13 | Whether the remote accepts a second concurrent bond | Pair to a second host, then re-pair to the TV | ❓ |

An nRF52 sniffer (Wireshark `bluetooth_hid` dissector) on a real TV ⇄ remote
session would close several of these at once.

Raw RE evidence for the firmware-derived claims: `REMOTE.md` (§4, §9).
