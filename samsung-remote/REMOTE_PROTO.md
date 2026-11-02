# Samsung Smart Remote — Protocol Reference (TV / host ⇄ remote)

Purpose: self-contained protocol documentation so that an agent can implement a
host (e.g. a computer) that talks to the Samsung Smart Remote, or instrument the
TV side. **All reverse-engineering evidence, addresses, and analysis notes live in
`REMOTE.md` — this file contains only protocol facts.**

Confidence markers used throughout:

- ✅ **CONFIRMED** — observed behavior or official documentation
- ◐ **INFERRED** — strongly supported by firmware behavior, not packet-verified yet
- ❓ **UNKNOWN** — needs to be filled in by live capture; listed in §9

Hardware context (facts only): the remote is a Realtek RTL8763B-family BLE SoC
(dual-mode BT, on-chip codec, I2S digital mic, USB-C charging). The TV (or a
replacement host) is always the **BLE central**; the remote is the **peripheral**.

---

## 1. Actors and transports

| Link | Transport | Notes |
|---|---|---|
| Pairing + keys + battery + all control | BLE, HID-over-GATT (HOGP) + Samsung vendor reports | Standard HID service plus vendor report IDs |
| Voice (mic audio) | BLE vendor report notifications (same HID/vendor GATT) | Audio fragments; codec TBD (§6) |
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
  - **Pairing the remote to a new host (e.g. a Mac) un-pairs it from the TV.**
  - Re-pairing to the TV later is always possible: hold Back+Play/Pause and let
    the TV connect.
- The remote uses a resolvable private address after bonding; the bonded central
  resolves it and reconnects automatically. An unbonded host only sees it while
  it is in pairing mode.

### 2.3 Post-connect handshake (what the TV does) — ◐ INFERRED

On a successful connect, the TV (central):

1. Enumerates GATT services and matches the advertisement/service data against
   its "smart remote" list (device classification, up to N bonded remotes).
2. Registers the vendor **report IDs** it cares about with the HID stack.
3. Stores remote identity/state (battery, firmware version, device type,
   capability, name) in its configuration keys (§8.1).
4. Accepts the remote's power-saving connection parameters (observed request:
   interval 20 ms, **peripheral latency 99**, timeout 8.1 s). Before voice
   capture it does not renegotiate them itself; it asks the remote to stop
   using latency with the HID command `34 31 1A 00 00` and reverts with
   `34 31 1B 00 00` (§6.2).
5. Queries device info with `31 01 17` (§6.2).

### 2.4 Recommended host-side pairing recipe — ◐ INFERRED

1. Power off nearby Samsung TVs (or move away) — the TV will grab the remote
   within seconds otherwise.
2. Put the remote in pairing mode (Back + Play/Pause 3 s).
3. Scan; connect to the advertising remote. Observed pairing-mode advert
   (✅ CONFIRMED, "Smart Control 2016", public address `38:8c:ef:…`):
   - ADV_IND (connectable, legacy PDU — no extended advertising):
     flags `0x05` (LE Limited Discoverable, BR/EDR not supported),
     appearance `0x0180` (Generic Remote Control),
     complete 16-bit UUIDs `1812` (HID), `180F` (Battery), `180A` (Device Info),
     peripheral connection interval range AD present.
     Raw: `02 01 05 05 12 10 00 20 00 03 19 80 01 07 03 12 18 0f 18 0a 18`
   - Scan response: complete local name `"Smart Control 2016"`.
   - **No manufacturer-specific data** (in particular no Samsung `0x0075`).
4. **Use Linux for the full GATT dump.** On macOS the HID service (`0x1812`)
   is hidden from apps by CoreBluetooth (§6.5) — you will only see `0x180A`,
   `0x180F`, and the vendor service `0xA001`. On Linux, disable the bluez
   `hog`/`input` plugins first so your client owns the Report
   characteristics.
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

**Consequence for a replacement host (ESP32 relay):** putting the board in
pairing mode (BOOT button) does nothing on its own — **the TV must be told
**to search. Options:

| Trigger | How | Notes |
|---|---|---|
| **Rooted TV (recommended)** | agent shell:
`app_launcher -f org.tizen.SmartRC -u 5001 type XF86BTPairing`
 | uid 5001 = owner (same pattern as the TV's own `appstore.sh`,
`art_start.sh`). Then press BOOT on the ESP32 while the search popup runs. |
| IR ✅ (used live) | send the TV's IR "BT pairing" scancode | captured with
the Flipper Zero from the real remote's Back + Play combo and replayed at
the TV: the search popup opens and the TV connects to the board within
~1 s of BOOT (2026-11-02). Scancode value not recorded here yet ❓ (in the
Flipper capture). The scancode lives in the micom MCU (`tztv-micom.ko`) |
| Settings menu | TV Settings → … remote/accessory pairing | exact menu path
not verified ❓; `org.tizen.SmartRC` is a partial-UI app, the menu entry may
just send the same key |

After bonding, the TV stores the "remote" (its Smart RC list, vconf keys
§8.1) and reconnects automatically whenever the board advertises (✅ live:
~1.5 s after the board starts advertising). On reconnection it reads nothing
from GATT: it reuses the PnP ID and layout cached at pairing. To make it
re-read them, the board must drop the bond so the TV pairs from scratch.

---

✅ CONFIRMED — dumped from a "Smart Control 2016" remote (fw type `EA`, version
`592200`) by the ESP32 receiver (`samsung-remote/esp32-receiver/`), after
bonding. Props: R read, W write, w write-without-response, N notify, I indicate.

| Service | Handles | Characteristic (value handle, props) | Notes |
|---|---|---|---|
| GATT `0x1801` | 0x0001–0x0004 | `0x2A05` Service Changed (0x0003, I) | |
| GAP `0x1800` | 0x0005–0x000d | `0x2A00` name (0x0007, R) = "Smart Control 2016"; `0x2A01` appearance (0x0009) = `0x0180`; `0x2A04` (0x000b); `0x2AA6` (0x000d) | |
| Device Info `0x180A` | 0x000e–0x0020 | `0x2A29` "Realtek BT", `0x2A24` "Model Nbr 0.9", `0x2A25` "RTKBeeSerialNum", `0x2A27` "RTKBeeHardwareRev", `0x2A26` "RTKBeeFirmwareRev", `0x2A28` "RTKBeeSoftwareRev", `0x2A23`, `0x2A2A`, `0x2A50` PnP ID | Realtek SDK placeholder strings, not real versions |
| Battery `0x180F` | 0x0021–0x0024 | `0x2A19` (0x0023, RN) | Read returned an empty value at connect |
| HID `0x1812` | 0x0025–0x003c | `0x2A4A` HID Info (0x0027); `0x2A4C` Control Point (0x0029, w); `0x2A4E` Protocol Mode (0x002b, Rw); `0x2A4B` Report Map (0x002d, 285 bytes); Report `0x2A4D` ×4 at 0x002f, 0x0033, 0x0037, 0x003a | See §4 for the report IDs |
| Vendor `0000d1ff-3c17-d293-8e48-14fe2e4da212` | 0x003d–0x0042 | `0xA001` (0x003f, wN); `0xA002` (0x0042, Ww) | Sends one all-zero 16-byte notification on subscribe. Not the voice channel (§6.5) |
| Vendor `9e5d1e47-5c13-43a0-8635-82ad38a1386f` | 0xff00–0xffff | `e3dd50bf-f7a7-4e99-838e-570a086c666b` (0xff02, WNI); `92e86c7a-d961-4091-b74f-2409e72efe36` (0xff05, Ww) | Probably Realtek DFU/OTA ❓ |

The **HID Report Map** (`0x2A4B`) is reproduced in §4 and is the authoritative
report catalog. It declares more report IDs than there are Report
characteristics; only the four in §4.0 exist on this remote.

---

## 4. Report catalog

All application traffic rides HID reports on the Report characteristics:
input reports arrive as notifications (remote → host), host → remote commands go
out as **Write Report / SET_REPORT**. The Report Map below is decoded from
firmware ✅; the "purpose" column combines the map with TV-side behavior.

### 4.0 Report characteristics — ✅ CONFIRMED (Report Reference descriptors, live)

| Value handle | Report ID | Type | Use |
|---|---|---|---|
| 0x002f | `0xF7` | Input (notify) | **Samsung key report**, §5.4. Every button arrives here |
| 0x0033 | `0xF6` | Input (notify) | no traffic seen yet |
| 0x0037 | `0xFB` | **Feature** (R W w, no notify; reads fail with ATT error `0xC3`) | **Command channel**, §6.2 |
| 0x003a | `0xFB` | Input (notify) | Command replies (e.g. Dev Info `33 …`, §6.2) and, by design, voice audio (§6.4) |

Over GATT the value carries **no report-ID byte**, in both directions: the
report ID lives in the Report Reference descriptor. Key notifications on
`0x002f` are 6 bytes (the report payload), and commands are written to
`0x0037` without a leading `FB` (§6.2).

### 4.1 Report Map (decoded) — ✅

| Report ID | HID page / usage | Payload | Direction | Purpose | Conf. |
|---|---|---|---|---|---|
| `0x01` | Keyboard (0x07): modifier byte + 6-key array | 8 B | Input | Standard keys (numbers, arrows modifiers) | ✅ |
| `0x02` | Mouse (0x01): 3 buttons + relative X, Y (int8) | 5 B | Input | Touchpad pointer motion | ✅ |
| `0x07` | Vendor 0xFF01 / usage 0x14 | 20 B | Input | Vendor input (touch/sensor family) | ◐ |
| `0x08` | Vendor 0xFF01 / usage 0x14 | 20 B | Output | Vendor output (touch/haptic/LED config family) | ◐ |
| `0xF6` | Consumer (0x0C), array-style | 180 B | Input | **Voice audio** (§6.4): 172-byte notifications, `<seq>` + 3 × 57-byte mSBC frames, 44.4/s while streaming. No key events seen on it | ✅ live |
| `0xF7` | Vendor 0xFF00 / usage 0x2F | 6 B | Input | **Samsung key report** (§5.4) | ✅ |
| `0xF8` | Vendor 0xFF00 / usage 0x2F | 6 B | Input | Vendor short telemetry | ◐ |
| `0xF9` | Vendor 0xFF00 / usage 0x2F | 18 B | Input | Dev Info (battery %, firmware version, device type ID, capability, language version) | ◐ |
| `0xFB` | Vendor | **192 B + 11 bits** | Input | Command replies (sub-ID in byte 0: Dev Info `33`, `60 03 00 00 00` ❓). **No audio** (audio is on `0xF6`) | ✅ live |
| `0xFB` | Vendor (Report Reference: Feature) | 2–5 B | **Feature (host → remote)** | **Command channel** — device info, voice session, latency (§6.2) | ✅ |
| `0x20` | *(not in Report Map; accepted via SET_REPORT)* | `20 <len> <data…>` | Output/Feature | **MBR command** (low-power/latency control); remote answers with an MBR_RETURN_REPORT and the TV exposes it via callback | ✅ |
| `0x23` | *(not in Report Map; accepted via SET_REPORT)* | `23 <data…>` ≤ 8 B | Output/Feature | **Haptic pattern** (haptic-capable remote types only) | ✅ |

Notes:
- Remote **hardware type ID** values seen in firmware: `0x20, 0xE1, 0xE2, 0xE4,
  0xE6, 0xE7, 0xE9, 0xEA, 0x05` (one per firmware variant).
- The TV registers a specific set of report IDs with its HID stack and logs
  `Ignore report Id = 0x%02x received` for others; the exact registered list is
  still to be captured (❓).
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

### 5.2 On a standard HID host — ◐

- Standard usages (Consumer Page: volume, arrows, transport; Keyboard page:
  number row) will be delivered by the OS like any HID remote.
- Vendor-report keys are ignored by the OS HID stack.
- **Linux (bluez, hog plugin disabled):** raw GATT notifications carry every
  report (report ID byte + payload); decode with the Report Map (§4.1) as
  reference. Press each button once to build the key→report map.
- **macOS:** CoreBluetooth will not show the HID service (§6.5); instead use
  IOHID-level APIs (`IOHIDManagerRegisterInputReportCallback`) to capture raw
  input reports — proven to work on the equivalent Apple remote (button
  packets incl. vendor report IDs arrive there). But note: **writes** back to
  the remote do not reliably reach its GATT characteristics from macOS, so
  host→remote commands (voice start, haptic, MBR) need Linux or the TV.

### 5.3 Host → remote commands — ✅ partially decoded

The host sends control via Write Report / SET_REPORT (see §4.1): MBR `0x20`,
haptic `0x23`, and the `0xFB` command channel (device info, voice, latency —
§6.2). Before a voice session the TV turns the remote's peripheral latency off
with an HID command (`34 31 1A 00 00`, §6.2), not with an LL connection
update. A low-power **MBR mode** can be enabled/disabled adapter-wide (TV CAPI
`bt_adapter_set_lpm_mbr_mode`).

Voice-key *press* on the remote is forwarded to apps as key code **462**
(`KEY_VOICECOMMAND`); the TV suppresses the matching *release* event while a
voice session is running. ❓ Exact byte encodings of remaining commands
(number-pad show/hide, voice-recognition-mode) still need a sniff.

### 5.4 Samsung key report (`0xF7`) — ✅ CONFIRMED (live)

Six bytes, notified on Report handle 0x002f:

```
press / auto-repeat:  CC 00 00 00 00 00
release:              00 00 00 01 00 SS
```

- `CC` = key code (e.g. `0xD2` = Power, **`0xA0` = MIC**). The press report is
  **resent every ~100–120 ms while the button is held**; a quick tap usually
  produces 2 press reports. **MIC is the exception: one press report, no
  auto-repeat**, then the release when it is let go.
- Release has byte 3 = `0x01` and byte 5 = `SS`, the **8-bit sum of the `CC`
  byte of every press report sent since the key went down**, i.e.
  `SS = (CC × n) mod 256` for `n` press reports. The host can use it to detect
  lost notifications. Example for Power: 1 report → `d2`, 2 → `a4`, 3 → `76`,
  5 → `1a`, 15 → `4e`.
- Verified on 64 key sessions with no mismatch. Other observed codes: `0x02`,
  `0x60`, `0x61`, `0x62`, `0x65`, `0x68`, `0x6a` (buttons not yet named; use
  `samsung-remote/esp32-receiver/keymap.sh`).
- Two buttons held together: the second key's press arrives without a release
  of the first, and the release checksum no longer matches the per-key sum.
  The multi-key encoding is not decoded yet ❓.

---

## 6. Voice / audio protocol

### 6.1 The mic is host-gated — ✅ CONFIRMED (this answers "holding mic does nothing")

**Holding the mic button alone does NOT start the mic.** The remote only sends
a voice-key *event*; the microphone stays closed until the host explicitly
commands a voice session. Confirmed four independent ways:

1. Live-tested on a Samsung TV (`tools/voice_poc.py --bt` /
   `SamsungTvMicCapture` payload): `bt_hid_rc_start_sending_voice()` and the
   rest of the CAPI chain return 0, but the callback got 0 bytes. The remote
   **does have a MEMS mic** (confirmed by teardown); the most likely reason is
   that the API was called without MIC held, which the remote ignores (point 3).
2. TV firmware sends a specific start command plus periodic extend/stop
   commands (strings: "start_sending_voice command", "Sending Voice command:
   0x%2x", "voice stop timer", "Voice key release, but streaming is coming!").
3. **The remote's own firmware gates the start on the MIC key** (REMOTE.md
   §9.6): pressing MIC opens a **voice window** that closes when all keys are
   released or after 1 s, whichever comes first. A start command (`31 04`)
   outside that window is **silently dropped**; inside it, the remote opens
   the mic and starts its stream timer. Releasing MIC after that does not stop
   the session.
4. Same architecture independently confirmed on Apple's Siri Remote
   (host-activated mic over HID-GATT; holding Siri alone produces zero audio
   reports) and on Android TV remotes ("hid-atv-remote" ADPCM-over-HID
   kernel driver) — this host-gated design is the industry pattern, not a
   Samsung quirk.

So on any non-Samsung host you must **send the start command yourself** (§6.2)
— and you must be on a platform that can reach the HID service at all (§6.5
has the macOS caveat that is easy to miss).

### 6.1.1 Voice transport generations — check yours before anything else ✅

Samsung has shipped **two incompatible voice transports** across remote
generations. Both are triggered by the same TV-side API
(`bt_hid_rc_start_sending_voice`), but the audio takes different paths:

| Generation | Remotes | Audio path | Reachable from a computer? |
|---|---|---|---|
| **A — classic-BT voice** | older mic-capable Smart Control
  (TV device types 58/65/66, pre-solar era) | **SCO/eSCO link**, PCM routed
  through the TV's kernel `btpcm` module (`/dev/btpcm0`) | ❌ **No.** eSCO is a
  classic-BT synchronous channel; macOS CoreBluetooth and plain bluez cannot
  establish it for an arbitrary HID device. Only the TV can do this. |
| **B — HID-report voice** | 2021+ solar remotes (incl. the 2025 Y25
  generation, Realtek RTL8763B) | **HID Input report `0xFB`** notifications,
  ADPCM-encoded, decoded by the TV's bluetooth daemon | ✅ Yes, via raw GATT —
  on Linux (§6.5). **Not macOS** (§6.5). |

How to identify which one you have: connect from Linux (not macOS) and dump
the GATT table. If the HID service (`0x1812`) has a Report characteristic with
Report Reference **report ID 0xFB** and a ~195-byte Input report — it is
generation B and this document applies. If the remote presents only
smaller reports (or is a classic-BT HID device rather than BLE) — it is
generation A and mic capture is TV-only. The 2025 firmware reverse-engineered
for this doc (`REMOTE.md`) is generation B. So is the "Smart Control 2016"
remote dumped in §3: the TV reports it as device type 49, yet it runs the same
Realtek Y25 firmware (type `EA`) and has the `0xFB` Feature/Input pair.

### 6.2 Command channel — framing and known commands — ✅ decoded (TV + remote firmware, live link)

All commands go to the **Feature report `0xFB`** (the Report characteristic
whose Report Reference descriptor says `0xFB / Feature`, HID report type 3;
handle 0x0037 on the dumped remote).

**Framing on the wire: the GATT value does NOT include the report ID.** The
TV builds `FB <payload>` and hands it to its HID stack as a Feature
SET_REPORT (`__bt_smart_rc_set_report`, logged as `Feature Report [FB3104]`);
the HID layer puts the report ID in the descriptor, not the value. Live on
the ESP32: `31 01 17` was answered, `FB 31 01 17` was ignored. Below, "bytes
written" is the GATT value.

```
<sub-ID> | <command> | [parameters…]
```

**Sub-ID** (byte 0) selects the handler in the remote's dispatcher:

| Sub-ID | Family | Conf. |
|---|---|---|
| `0x31` | device info query + voice session control | ✅ |
| `0x34` | link/feature control; **byte 1 must be `0x31`** or the write is dropped. Commands (byte 2): `04,05,06,07,1A,1B,20,21,22,30,31` | ✅ (list) / ◐ (roles) |
| `0x20`, `0x22` | MBR / low-power control | ✅ |

**`0x31` commands** (byte 1):

| Bytes written | Meaning | Conf. |
|---|---|---|
| `31 01 17` | **Device info query** (TV: `__bt_service_fetch_hid_device_info`, sent after connect). The remote requires byte 2 = `0x17` and answers on the `0xFB` Input report with sub-ID `0x33`, e.g. `33 17 00 03 00 00 02 00 02 31 a3 00 00 00 ea 59 22 00 11` (contains the firmware type `EA` and version `59 22 00`, matching `SC_BT_Y25_EA592200`). No audio. | ✅ live + both firmwares |
| `31 04` | **Start sending voice** (TV voice-key handler, `__bt_send_voice_cmd(4)`). Accepted only inside the MIC voice window (§6.1); otherwise silently dropped. No reply. | ✅ live: audio on `0xF6` ~180 ms later |
| `31 03` | Extend (voice event 1: restarts the stream timer while streaming). The TV sends `31 03 10` ~250 ms after `31 04` | ✅ remote fw + live |
| `31 02` | **Stop** (TV: key release / stop timer, `__bt_send_voice_cmd(2)`) | ✅ both firmwares |
| `31 06` | Stereo voice start on the TV side; **not handled by this remote** | ✅ |
| `31 24 <byte>` | Parameterized command; plausibly voice-recognition mode. The TV sends `31 24 10` after connect | ◐ (seen live) |

**`0x34` commands used by the TV:**

| Bytes written | Meaning | Conf. |
|---|---|---|
| `34 31 1A 00 00` | **No-latency on** (TV `__bt_set_ble_conn_no_latency(TRUE)`), sent before voice | ✅ both firmwares |
| `34 31 1B 00 00` | **No-latency off**, sent after the session | ✅ both firmwares |
| `34 31 07 00 00` | Sent by the TV first thing after (re)connect; meaning ❓ | ◐ seen live |

Live, through the ESP32 relay (2026-11-02), the TV sent **no** `34 31 1A` /
`1B` around a MIC session: only `31 04`, `31 03 10`, `31 02`.

Behavioral notes for implementers:

- The remote **silently drops** anything outside the accepted set: wrong
  sub-ID, `34` without `31` in byte 1, `31 01` without `17`, or `31 04`
  outside the MIC window. No error response, just nothing.
- Only `31 01 17` produces a reply. Every write is still ATT-acknowledged, so
  "write succeeded" does not mean "command accepted".
- The TV re-sends start as the *first* command after the remote wakes
  ("Sending start voice command from SC wake up Always must be first"), and
  tracks a pending flag and a stop timer around each send ("voice_cmd_pending",
  "voice stop timer", "Reset voice stop timer").

### 6.3 Session flow (reference behavior, modeled on the TV)

```
(after connect: host → remote  31 01 17   device info query, answered by 33 …)

User presses MIC on the remote
  1. remote sends the MIC key report (0xF7: A0 00 00 00 00 00, no repeats)
     and opens its voice window (until all keys are released, max 1 s)
     (apps on TV see key code 462 = KEY_VOICECOMMAND; release event is
     suppressed while a session is running)
  2. host → remote: 34 31 1A 00 00   no-latency on
  3. host → remote: 31 04            start — must arrive while the window is
                                     open, i.e. while MIC is still held
  4. remote → host: report 0xFB Input notifications — audio frames (§6.4)
  5. host → remote: 31 03 (extend) while the session should continue
  6. key release / host stop timer → host → remote: 31 02 (stop)
  7. host → remote: 34 31 1B 00 00   no-latency off
```

Status (2026-11-02 16:27, ✅ **SOLVED**, ESP32 receiver, `EA`/`592200` remote):
the session works, run by the board alone with the TV's exact sequence.
**The earlier "no audio" was a host-side mistake:** the audio was always
there, but on report **`0xF6`**, not `0xFB`. The receiver decoded `0xF6` as
Consumer-page keys and only looked for audio on `0xFB`. Working sequence
(hold to talk):

```
MIC down  → remote sends 0xF7 A0 00 00 00 00 00
host      → 31 04                (immediately, while MIC is held)
remote    → 0xF6 audio, first report ~180 ms after the start
host      → 31 03 10             (~250–500 ms after the start, once)
MIC up    → remote sends 0xF7 00 00 00 01 00 A0
host      → 31 02                (stop; a report or two may still arrive)
```

Steps 2 and 7 of the reference flow (`34 31 1A/1B`) are not needed: the TV
doesn't send them, and the stream runs at 44.4 reports/s without them.

TV-side session state (strings in the TV daemon — replicate this logic if you
want robust behavior):

- `is_pressing` / `is_voice_sending` flags; "voice key press already sent once"
  (dedupe), "voice key press was not sent earlier, discard voice key release"
  (spurious releases are dropped), "Voice key release, but streaming is
  coming!" (stop was sent but frames still arrive — keep draining),
  "voice streaming is started/stopped" state transitions, and a
  `voice_exception_timer` safety net.

### 6.4 Audio frames (report `0xF6` Input, mSBC) — anatomy ✅

> **LIVE 2026-11-02 16:15, ESP32 relay (board log
> `esp32-receiver/logs/f6-first-stream.log`): the mic stream arrives on report
> `0xF6` Input (h=`0x0033`, 180 bytes), NOT on `0xFB`.**
>
> - **First stream:** 260 ms after the TV's first `31 04` (tvctl
>   `record_remote_mic.sh`, relayed by the board), the remote started
>   notifying `0xF6`. It also streamed on each later MIC press, when
>   bt-service sends `31 04`, then `31 03 10`, and `31 02` on release.
> - **No audio on `0xFB`:** there were no `0xFB` Input frames at all. The
>   only `0xFB` traffic was the replies `33 17 …` (device info) and
>   `60 03 00 00 00` (new sub-ID `0x60`, ❓ meaning).
> - **Why the board missed it:** it logged each `0xF6` report as ~180
>   Consumer-page "key presses" and counted only `0xFB` as audio, so it
>   reported "0 audio frames". The flood of key lines also slowed the board
>   (~7 reports/s seen; the true rate is unknown).
> - **Board fix (flashed):** `0xF6` Input now goes to the voice logger as
>   `voice: FRAME` hex lines, the format `voice_capture.py` reads.
> - **TV result:** in that run the TV captured 0 bytes. The cause is still
>   open ❓: the board slowdown above, or the TV expecting something else.
> - **Still to measure:** frame layout, rate, and whether the first byte is
>   a header.
>
> **✅ DECODED (16:19, second run, board log `esp32-receiver/logs/latest.log`
> of that session): the codec is mSBC, not IMA-ADPCM.**
>
> - **Report layout:** every `0xF6` notification is **172 bytes**:
>   `<seq> | 3 × 57-byte mSBC frames`.
> - **Sequence byte:** byte 0 is a counter that increments per report; there
>   were no gaps in 197 reports.
> - **mSBC frames:** each one starts with the mSBC sync header `AD 00 00`, at
>   offsets 1, 58 and 115 in every report. mSBC is 16 kHz mono with 120
>   samples per frame, so one report is 360 samples = 22.5 ms, i.e. 44.4
>   reports/s. Measured: 44.5/s.
> - **Decode:** strip byte 0, concatenate the rest, then run
>   `ffmpeg -f sbc -i x.msbc -ar 16000 x.wav`. Result: 4.43 s of 16 kHz audio,
>   mean level -20.8 dB.
> - **Timing:** the stream started 180 ms after MIC was pressed while tvctl
>   was retrying `31 04` every 300 ms. It stopped on MIC release, when the TV
>   sent `31 02`.
> - **The TV still captured 0 bytes.** The board also lost the TV link during
>   streaming (`TV disconnected; reason=0x222`, LL response timeout), most
>   likely because the board is loaded by relaying plus logging 44 hex lines
>   per second.
>
> **The start gate, measured (both runs, `31 04` retried every 300 ms by
> tvctl):**
>
> - **No key held:** in run 2, `31 04` was sent for 23 s with no key held,
>   and the remote produced **0 audio**. The board cannot start the mic on
>   its own.
> - **Not only MIC opens the window:** in run 1, a `31 04` write landed
>   while **Down** (`0x61`) was held, from 51056 to 51156. The `0xF6` stream
>   started at 51236, with no MIC press at all. So it seems any held key
>   opens the window, not only MIC. This is a single observation, ◐.
> - **Releasing the key does not stop the stream:** while the host keeps
>   resending `31 04`, the stream keeps running. One Down tap gave
>   51.2–64.7 s, and one MIC hold gave 68.6–81.2 s; that stream ended with
>   `31 02`, 9 s after the release.
>
> TV-side command sequence observed through the relay:
>
> - at connect: `34 31 07 00 00`, then `31 01 17`, then `31 24 10`
> - on MIC: `31 04`, then `31 03 10`, then `31 02`
>
> **✅ Board-driven capture (16:27, `SRR_VOICE_LOCAL`):** the board sends the
> same three commands itself on MIC down / up, and the TV gets neither the
> MIC key nor the audio. Two sessions: 148 and 142 reports, 44.5 reports/s,
> no sequence gaps, audio confirmed correct by ear. Host side:
> `esp32-receiver/voice_capture.py` follows the monitor log and writes one
> WAV per MIC press (mSBC → 16 kHz s16le via ffmpeg).

**Superseded pre-capture hypothesis (kept for the record; wrong about the
report and the codec):**

Per the Report Map, each `0xFB` Input report carries **192 bytes of 8-bit
fields + 11 one-bit fields** (padded to 194 content bytes). Over GATT the
notification value is the payload only (no report-ID byte, §4.0).

The TV's incoming-report dispatcher reads the **first payload byte as a
sub-report ID** (handled range `0x1F`–`0x34`; the Dev Info reply uses `0x33`),
so an audio frame is expected to look like:

```
<sub-ID ❓> | ADPCM audio … | flag bits …       (GATT value, no report ID)
```

- **Strip the sub-ID byte before decoding** — its value for audio is not known
  yet (❓ needs one capture).
- Remaining payload ≈ 192–193 bytes: **IMA-style ADPCM mic audio** (the TV
  decoder keeps an IMA step-table index across frames, `adpcmIndex`) plus the
  **11 flag bits** (❓ purpose — plausibly VAD/speech-detection metadata).
- At IMA-ADPCM's 2 samples/byte: ~192 B ≈ 384 samples ≈ **24 ms of audio** per
  notification at 16 kHz → expect **~42 notifications/second** while
  streaming. If frames arrive at a different cadence, the payload split
  differs — measure, then update this table.
- Host-side delivered format (after TV decode): **16 kHz, mono, signed 16-bit
  LE PCM**; the TV callback receives `{int16 length; void* audio_buf}` per
  chunk. (Analogous known-good system: Android TV remotes stream ADPCM in HID
  reports at the same order of frame rate.)

Decoding for verification (Python, no external deps):

```python
# IMA-ADPCM decode, per channel state (index + predictor)
STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
        50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,
        230,253,279,307,337,371,408,449,494,544,598,658,724,796,
        876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499]
INDEX_TABLE = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]

def decode_ima(data, predictor=0, index=0):   # data: nibble stream
    out = bytearray()
    for i, b in enumerate(data):
        for nib in (b & 0xF, b >> 4):
            step = STEP[index]
            diff = step >> 3
            if nib & 1: diff += step >> 2
            if nib & 2: diff += step >> 1
            if nib & 4: diff += step
            predictor += -diff if nib & 8 else diff
            predictor = max(-32768, min(32767, predictor))
            index = max(0, min(88, index + INDEX_TABLE[nib]))
            out += predictor.to_bytes(2, 'little', signed=True)
    return bytes(out), predictor, index
# feed each 0xFB frame's audio payload; state carries across frames
```

If the decoded audio sounds pitched/wrong, try: swapping nibble order,
16 kHz→8/32 kHz, or a different payload offset (the first bytes of a frame may
be a per-frame header, not audio).

**Format summary:**

- *(superseded: the link codec is mSBC on `0xF6`, see the top of §6.4)*
- **Link codec: ADPCM** (TV decodes with an IMA-style ADPCM decoder —
  step-table state kept across frames, so frames must be decoded **in order**
  and state must not be reset mid-stream).
- Each report `0xFB` Input notification carries ~**192 bytes** of ADPCM data
  (map declares 192 bytes + 11 bits, §6.4).
- IMA-ADPCM packs 2 samples/byte: 192 B ≈ 384 samples ≈ **24 ms** per
  notification at 16 kHz (≈42 notifications/s).
- Host-side delivered format: **16 kHz, mono, signed 16-bit little-endian PCM**
  (callback struct `{int16 length; void* audio_buf}`).
- Sample rate/bit-depth on the link are derived, not packet-verified ❓ — confirm
  by playing a captured 0xFB stream through the decoder in §6.4.

Alternate audio paths the same link supports (TV side exposes them as separate
register/decode paths): **stereo voice** (dual-mic remotes), **far-field voice**
and **far-field voice with noise** variants. If a remote streams on one of
those instead, frames will still arrive on `0xFB` but with different length /
multi-mic layout ❓.

### 6.5 Capturing on a non-Samsung host — platform guide

#### macOS: **blocked — read this before trying again** ❌

**macOS cannot reach the voice command channel. At all.** This is almost
certainly why the Mac attempt produced nothing:

- **CoreBluetooth does not expose the HID service (`0x1812`) to third-party
  apps.** A connected/bonded HID-over-GATT device shows only non-HID services
  (e.g. Device Information `0x180A`, Battery `0x180F`, and the vendor service
  `0xA001`). The Report characteristics — the only place the §6.2 commands
  can go — are invisible. This was verified experimentally on the same device class
  (Apple Siri Remote: `discoverServices([0x1812])` returns nothing even when
  the remote is unbonded and connected directly as an ordinary BLE
  peripheral — CoreBluetooth filters the HID service under all circumstances).
- macOS's kernel HID stack owns the connection, rewrites report IDs into
  per-interface IOHID devices, and owns the CCCD state. IOHID `SetReport` calls
  do not map onto the remote's actual GATT Report characteristics (Apple's
  own remote needs a DriverKit replacement to work around this; it cannot be
  done from userspace on stock macOS).
- **If you saw the `0xA001/0xA002` vendor service and wrote there — that is
  not the voice channel.** The vendor characteristic's write handler in the
  remote firmware expects ≥16-byte structured payloads (sync/OTA family),
  not the 3–4-byte voice commands.
- bleak on macOS uses CoreBluetooth → same wall. There is no flag, entitlement
  or workaround for stock macOS.

Do not spend more time on macOS. Use one of the working platforms below
(ESP32 first — it is the best fit for this capture).

#### ESP32: ideal platform — full raw GATT client ✅ (recommended for the capture)

An ESP32 is the best capture host for this: nothing claims the HID device, and
the NimBLE host gives complete raw GATT client access (all services including
`0x1812`, all characteristics, all descriptors). Works on classic ESP32 (BLE 4.2)
and ESP32‑C3/S3 (BLE 5.x). **Three ESP32-specific pitfalls explain most
"nothing happens" failures — check them in this order:**

1. **Pair + bond first — HID writes require an encrypted link.** The remote
   (like all HOGP devices) enforces security on the HID service: writes from
   an unencrypted connection fail (or are silently dropped). Set IO capability
   `NoInputNoOutput` (Just Works), initiate pairing **as central** right after
   connecting, and accept the bonding event. Store the bond — the remote keeps
   exactly one bond.
2. **Request a large MTU before anything else.** Default ATT MTU is 23 (20-byte
   payloads); the `0xFB` audio frame is ~195 bytes and would arrive fragmented
   or not at all through a stack that can't reassemble. Call the MTU-exchange
   (e.g. NimBLE: `ble_att_set_preferred_mtu(247)` + negotiate on connect;
   NimBLE‑Arduino: `client->setMTU(247)`).
3. **Write to the *Feature* 0xFB Report characteristic, without a report-ID
   byte, while MIC is held**: `31 04` (then `31 03 10`; `31 02` on release),
   write-with-response (§6.3). **Subscribe to `0xF6` Input: the audio
   comes there**, not on `0xFB`. Writing to the *Input* 0xFB char or to the
   vendor `0xA002` char does nothing (different handlers — REMOTE.md §9.6).

Working implementation: `samsung-remote/esp32-receiver/` (ESP-IDF + NimBLE).
It scans for the advert in §2.4, bonds Just Works, negotiates MTU 247, dumps
the GATT table and Report Map, subscribes to every notifying characteristic,
decodes the `0xF7` key report, and runs the §6.3 session while MIC is held,
logging the `0xF6` audio reports for `voice_capture.py` (one WAV per press).

Extra ESP32 notes:

- The remote runs with peripheral latency 99 (it may skip ~2 s of connection
  events while idle). It answers immediately after a key press, which is when
  voice commands are sent anyway.
- To re-pair to the TV later: hold Back+Play/Pause and let the TV connect.
- If discovery shows fewer characteristics than expected, the connection
  probably isn't bonded/encrypted — reconnect and re-check (HID hides nothing
  at GATT level, but the remote may gate attributes on security).
- For ground-truth cross-checks, an nRF Sniffer alongside the ESP32 session
  resolves any remaining byte-level questions.

#### Linux: the reliable path ✅

On Linux you can stop the BlueZ HOG plugin from claiming the device and then
own the raw GATT table (the same trick the Linux Siri-remote projects use):

```bash
# 1) prevent bluez from claiming the HID device (pick one):
#    /etc/bluetooth/main.conf  →  [General] DisablePlugins=input,hog
#    (or run bluetoothd manually: sudo /usr/lib/bluetooth/bluetoothd -n -d --noplugin=input,hog)
sudo systemctl restart bluetooth

# 2) put the remote in pairing mode: hold Back + Play/Pause ≥ 3 s (LED blinks)
#    (this drops the TV bond — the remote keeps ONE bond at a time)
bluetoothctl
  [bluetoothctl] scan on          # look for the Samsung advertisement
  [bluetoothctl] agent on
  [bluetoothctl] default-agent    # NoInputNoOutput / JustWorks
  [bluetoothctl] pair <MAC>
  [bluetoothctl] connect <MAC>

# 3) enumerate services with a raw GATT client (btgatt-client, or python3 +
#    bleak which uses bluez on Linux):
#      - find ALL Report (0x2A4D) characteristics and read each 0x2908
#        Report Reference descriptor
#      - the char with (report ID 0xFB, type 3 Feature)  → command channel
#      - the char with (report ID 0xF6, type 1 Input)   → audio (enable CCCD)
#    Check the channel first: write 31 01 17 to the Feature char; a 33 …
#    Dev Info reply on the Input char confirms it.

# 4) press and hold MIC, then write 31 04 (hex) to the Feature char before
#    releasing it; write 31 02 on release; record everything.
```

Expected result (seen on the ESP32, §6.3): `0xF6` notifications of 172 bytes
at 44.4 per second (mSBC, §6.4). If nothing comes: check the writes went to the
**Feature** 0xFB char (not Input), carry **no** leading `FB` byte, and that
the start was sent while MIC was still held.

#### Rooted Samsung TV: **the voice trigger** — no Bixby login needed ✅

The TV can be told to start a remote-mic session **without the voice
assistant**: the Bluetooth CAPI is directly callable, and the voice stack
(STT/Bixby) is never involved.

**How to run TV-side commands: `./shell.sh` (repo root).** It opens an
interactive root shell on the TV through this lab's private channel. Two
rules for agents:

- **Never look inside `shell.sh` or the tooling it calls** — it is private
  lab infrastructure and reading into it trips safeguards. Treat it as a
  black box.
- **If it misbehaves, do not debug it — ask the human.**

For non-interactive use, the same channel provides one-shot commands and
file transfer: `python3 tvctl/tvctl.py exec "<cmd>"`, `… pull <remote>
<local>`, `… push <local> <remote>` (re-establish with `… activate <tv-ip>`
after a TV power cycle). Batch/long commands: wrap in `timeout` and pull
results as files — the interactive shell is serial and single-session.

**Recording the remote mic:**

```sh
./tools/record_remote_mic.sh 15        # 15 s; hold MIC on the remote during it
# → tmp/remote_mic.pcm + tmp/remote_mic-chunks.log on this machine
```

It stages the payload at `/run/samsung-tv-root/mic/` on the TV (`/run` is
tmpfs — stage again after a TV power cycle), runs it, and pulls the PCM
plus a per-chunk timing log back. Plain commands, for reference:

```sh
python3 tvctl/tvctl.py exec "cd /run/samsung-tv-root/mic && \
  /usr/bin/dotnet SamsungTvMicCapture.dll <seconds> <out.pcm>"
python3 tvctl/tvctl.py pull <out.pcm> ./tmp/remote_mic.pcm
```

Verified live (2026-11-02 16:04, Q7F3, ESP32-relay era): the whole CAPI
chain runs and the TV addresses its Smart RC by the board's MAC
(`smart control address: 0C:4E:A0:61:B8:0D`, device type 49),
`bt_hid_set_audio_data_receive_cb -> 0`, and the payload retried `start`
10× in a 3 s run. Zero bytes arrived **because the remote was bonded to
the ESP32, not to the TV** — the TV sent `31 04` into a relay whose
remote-side link was up, but the ESP32⇢remote half is where the stream
would have to come back through (see the MITM section for that state).
Two working topologies:

1. **Remote paired directly to the TV** (hold Back+Play/Pause near the TV):
    the TV's own BLE link carries the commands and the audio — this is the
    plain trigger, and the only one that yields audio **today**.
2. **Remote paired to the ESP32 relay**: the TV's `31 04` reaches the remote
    through the relay (once the §6.5 UUID fix is flashed); the audio
    returns TV-ward through the same relay.

The payload itself (`payloads/common/SamsungTvMicCapture.cs`):

1. `bt_initialize` → `bt_audio_initialize` → `bt_hid_host_initialize` →
   `bt_hid_set_audio_data_receive_cb` (audio arrives as 16 kHz mono s16le
   in the callback — verified live, all calls return 0; the occasional
   `bt_hid_host_initialize -> -22` when bt-service already owns the HID
   host is harmless).
2. **Retries `bt_hid_rc_start_sending_voice` every 300 ms** for the whole
   run — critical, because the remote drops `start` unless MIC is held
   (§6.1). Hold MIC any time during the run and the retry inside the window
   arms the stream; then periodic `extend` keeps it alive.
3. Writes the PCM + a per-chunk timing log (`<out>.log`: timestamp, length,
   first bytes of the first chunks — resolves the §6.4 cadence and
   frame-header questions).

If audio still doesn't flow with MIC held, run the trigger while capturing
`vlogutil` (bt-service logs go to **vlog**, not dlog) — it prints every
command it sends (`Sending Voice command: 0x%2x`, the full hex `Feature
Report [%s], result = %d`, `hid not ready`), which shows exactly what the
TV sends or refuses.

#### MITM: remote → ESP32 → TV (the diff machine)

For the definitive answer, put the ESP32 between remote and TV so a **real**
TV session can be observed (and the missing condition found by diffing it
against our manual sequence):

- **ESP32 = central to the real remote** (pretends to be the TV): existing
  `samsung-remote/esp32-receiver/` firmware already does this half — remote
  bonds to the ESP32.
- **ESP32 = peripheral to the TV** (pretends to be a remote): a NimBLE GATT
  *server* mirroring §3's table — HID service with the four Report chars
  (report IDs from the `0x2908` descriptors: `0xF7` keys, `0xF6`, `0xFB`
  Feature, `0xFB` Input), Battery, Device Info, and the `d1ff…` vendor
  service. Serve the real remote's 285-byte Report Map so the TV's HID stack
  accepts it. **To get the TV to bond to the board, trigger the TV's remote
  search per §2.5** — the board's advert alone does nothing.
- Run the **voice trigger** (previous subsection) on the TV: the TV now sends
  its real command sequence to the ESP32, which (a) logs every byte, (b)
  relays them to the real remote, (c) relays the remote's reports
  (incl. `0xFB` audio if it streams) back up to the TV, and (d) keeps a copy.
- Diff TV-sequence vs our manual `34 31 1A 00 00` + `31 04`: any extra/missing
  command, ordering, or timing difference **is** the missing condition.
- NimBLE supports the required multi-role (one central connection + one
  peripheral) on a single radio; the traffic budget is trivial (~8 kB/s of
  audio at §6.4 cadence).

Even simpler first pass: skip the peripheral emulation and just run the voice
trigger while the remote is paired to the TV normally — the dlog capture alone
may answer everything.

#### MITM relay — status (esp32-receiver `main/tv.c`, 2026-11-02)

**Implemented** (ESP32-C3, ESP-IDF 6.1, NimBLE central + peripheral):

- GATT server copying the remote's **full table, handle for handle**
  (since layout v2, see "Fix" below). It contains:
  - `1801` with only `2A05` and its CCCD
  - `1800` with name "Smart Control 2016", appearance `0x0180`, PPCP
    `10 00 10 00 63 00 2a 03` and an empty `2AA6`
  - `180A` with all nine characteristics and the remote's values (Realtek
    placeholder strings, System ID `00 01 02 00 00 03 04 05`, PnP ID)
  - `180F`
  - `1812` with HID Info `11 01 00 00`, Control Point, Protocol Mode, the
    285-byte Report Map (§4.1), and the four Report chars with their Report
    Reference descriptors: `0xF7`/`0xF6` Input, `0xFB` Feature, `0xFB` Input.
    The Input reports are R/W/N, as on the remote.
  - `d1ff…` (A001/A002), then `9e5d1e47…`

  Every handle up to `0x0042` matches the remote. The only exception is
  `9e5d1e47…`, which starts at `0x0043` instead of `0xff00`, because NimBLE
  assigns handles sequentially. At boot the board compares its handles with
  the remote's and logs `GATT layout matches the remote's handles`. TV
  writes to the vendor characteristics are logged, not forwarded. All HID
  attributes require encryption.
- Advert: the remote's pairing-mode advert byte for byte (§2.4), flags
  `0x05` while pairing and `0x04` for reconnection, 30 ms interval.
- PnP ID: `SRR_TV_PNP_ID`, empty by default = the remote's own PnP ID with
  product ID + 1 (`005D:0002`); tested live with `02e80475200100`
  (Samsung `04E8:2075`).
- Pairing: BOOT short press while the remote is connected → deletes the
  board's TV bond → advertises in pairing mode for 60 s. The TV's search
  must be running (§2.5, IR from the Flipper). BOOT held 3 s erases all
  bonds and the saved TV address.
- Relay remote → TV: every input report is notified to the TV once the TV
  link is encrypted, **whether or not the TV subscribed** (it never does,
  see below). Battery forwarded.
- Relay TV → remote: `0xFB` Feature writes are forwarded to the remote and
  logged as `voice: TV->RC …`; a TV `31 04` opens a logged voice session
  (`voice_capture.py` input; with `SRR_VOICE_LOCAL`, the default since
  16:27, the TV's voice commands are dropped instead).
- Every TV GATT read, write and CCCD change is logged (`tv:` lines).

**Live results (layout v1, before the fix below):**

| Step | Result |
|---|---|
| TV search (Flipper IR) + BOOT | ✅ TV connects ~1 s after BOOT, pairs Just Works, MTU 247 |
| TV reconnection | ✅ automatic while the board advertises |
| TV GATT accesses after a fresh pairing | reads PnP ID → HID Info → Report Map (13 blob reads at MTU 23) → MTU exchange, writes the Service Changed CCCD (`0x0008`) = 0. **Then nothing** |
| Report Reference descriptors | ❌ never read |
| Report CCCDs (`0xF7`, `0xFB` Input, …) | ❌ never written |
| Keys relayed to the TV (notified without CCCD) | ❌ TV ignores them |
| TV → remote commands (`31 01 17`, `34 31 1A`, `31 04`) | ❌ none seen: the TV never writes to `0xFB` Feature |

Same behaviour with PnP `005D:0002` and `04E8:2075`, so the PnP ID is not
what stops the TV's HID setup. The TV stops right after the Report Map,
before learning which characteristic carries which report ID.

**DIAGNOSIS COMPLETE (2026-11-02, via `shell.sh` + `vlogutil` — this is why the TV
ignores everything):** after the power-cycle reconnect, the TV's BT stack
has the ESP32 bonded and marked connected (vconf `is_smart_rc_paired=True`,
`bt_smart_rc_connected=True`, name/addr/version/battery all set — the TV even
parsed the cached dev-info `0xEA592200`), but its **HID host state machine
never became ready**. The vlog (`vlogutil -t 7200 -v kerneltime`, bt-service
tags go to **vlog, not dlog** — `dlogutil` shows nothing) says, repeated
**every 2 s forever**:

```
bt-service: _bt_service_product_hidhost_is_ble_rc(686) > Device is BLE RC
bt-service: __bt_smart_rc_make_le_report_as_str(2565) > +        (builds the dev-info query)
bt-service: oal-hid-host.cpp: hid_set_report(120) > len: 8            ("31 01 17" hex string)
bt-host:   hid-host-q.cpp: SetReport(204) > [ERROR]hid not ready      ← THE BUG
bt-service: _bt_send_rc_le_command(2656) > Send report failed for: [0C:4E:A0:61:B8:0A], error: [-29]
bt-service: __bt_service_fetch_hid_device_info(2496) > query device info request failed
```

So the TV **is trying** to send `31 01 17` every 2 s — the command channel
isn't being "ignored", it's **failing** with `hid not ready` (error `-29`,
"IN Progress — wait retry from APP"). Meanwhile the relayed keys DO reach
the TV's GATT layer (btsnoop shows the `0x61` notification, handle 35) but
land as `bta_gattc_check_notif_registry … RFN not matched` — **no GATT client
registered for the notification**, because the TV never wrote the report
CCCDs (they're written when HID setup completes, which it never does).

**Root cause chain:** `HidHostQ::setReadyStatus()` gates every command;
the ready flag is set from HID-host connect callbacks
(`cb_registered_report` / `cb_mtu_configuration_complete` in
`libbt-oal.so`'s `hid-host-q.cpp`) that fire when the **stack-level HID
profile connect** completes. For HOGP the profile connect completes only
after a full GATT discovery + report registration — which is exactly the
step that stalls (no Report-Reference reads → no report registration → no
ready → the 2 s retry loop can never succeed → CCCDs never written → keys
ignored). The stuck state also explains why the earlier pairing session
looked "accepted" (PnP/HID-info/Report-Map reads) but then froze: the
profile connect never finished.

**Most likely triggers to check, in order (for the esp32-receiver agent):**

1. **GATT table mismatch with the bond's cached attributes.** The TV bonds
   and then does GATT discovery; on the *power-cycle reconnect* it likely
   reuses the cached handle table. Two concrete mismatches vs the real
   remote: (a) the board serves **`180A`/`180F`/`1812` only** — no `1801`
   Service Changed, no `1800` GAP service (the real remote serves both,
   see §3); (b) **service/handle order differs** (board: `180A` first;
   real remote: `1801`,`1800`,`180A`,`180F`,`1812`). If the cached table
   says e.g. report `0xF7` = handle `0x002f` (the real remote's) and the
   board's table doesn't match, discovery aborts and HID never registers.
   Fix: serve the full table in the real remote's exact order (`1801` with
   `2A05`+CCCD, `1800` with `2A00` name / `2A01` appearance / `2A04` /
   `2AA6`, then `180A`, `180F`, `1812`), and/or make the TV re-discover by
   changing the table (bump a Service-Changed indication or re-pair fresh
   after any handle layout change).
2. **Service Changed handling on reconnect.** The real remote's `0x2A05`
   (Service Changed, indicate) exists so the peripheral can tell the central
   "attributes changed, redo discovery". The board doesn't serve `0x1801`
   at all — on reconnect after the pairing session (where handles may have
   been cached mid-setup), the TV may be using a stale/incorrect cache with
   no way to learn it changed.
3. **The 13 Report-Map reads at MTU 23.** During pairing the TV read the
   285-byte Report Map in 13 × 23-byte ATT reads *before* requesting MTU
   247. If the stack expects Report-Reference discovery to follow the
   *MTU-exchange callback*, and that ordering broke (MTU exchange arriving
   after the reads instead of before), the profile-connect completion
   callback may never fire. Real remote does MTU exchange right after
   connect, before HID reads — the board should try to force/reject that
   ordering (e.g. make the Report Map characteristic >512 B, or delay reads).
4. **Stack-level error during discovery.** `gatt_get_link_encrypt_status
   status=0x8d` appears in the btsnoop stream around the relayed
   notifications — `0x8d` = GATT_INSUF_ENCRYPTION-ish/`ERR_CL_LT_` class;
   verify every HID attribute the TV touches is actually
   encryption-permitted in NimBLE (`ENC_R`/`ENC_W` flags vs
   read-with-encryption semantics; a wrongly *un*encrypted attribute on a
   bonded link is fine, but an attribute requiring MITM the TV can't
   provide would abort discovery).

Fastest verification path (no re-pairing needed): watch the TV vlog while
forcing a GATT cache reset — on the TV,
the one-shot command channel (`shell.sh` infra) running `vlogutil -t 120 -v kerneltime -o /tmp/v.txt …` while
toggling the board (or `rm` the TV's GATT cache for the peer if present in
`/opt/var/lib/bluetooth/…` — needs a fresh look at that dir layout), then
press keys and see whether `hid not ready` flips to "HID Remote Control
Connected" + `Register report ids`.

**Corrections to the list above, from the board's own logs (esp32-receiver,
2026-11-02):**

- **Item 1(a) is wrong; item 1(b) is right.** Layout v1 *did* serve `1800`
  and `1801`: the TV wrote the Service Changed CCCD at h=`0x0008`. They came
  from NimBLE's built-in services, which register GAP before GATT. NimBLE's
  `1801` also adds `2B3A` (Server Supported Features) and `2B29` (Client
  Supported Features), which the remote doesn't have. `180A` had only 3 of
  the remote's 9 characteristics, and both vendor services were missing.
  Every handle was therefore shifted. For example, the `0xF7` report value
  was at h=`0x0023` instead of `0x002f`; that is the "handle 35" in the
  TV's btsnoop.
- **Item 4:** in Bluedroid, `0x8d` is `GATT_ENCRYPED_NO_MITM`, meaning the
  link is encrypted without MITM protection. That is normal for Just Works,
  and the real remote pairs the same way. It is a status, not a failure.
- **Item 3:** not addressed. The TV starts the MTU exchange, and the board
  doesn't.

**Fix: layout v2 (flashed 2026-11-02, ❓ awaiting a fresh TV pairing)**

1. **Exact table.** The board no longer calls `ble_svc_gap_init()` or
   `ble_svc_gatt_init()`. It defines `1801` and `1800` itself, in the
   remote's order, followed by every other service. Handles up to `0x0042`
   are identical to §3. This was verified at boot.
2. **Service Changed.** NVS key `tv_gatt` stores the layout version the
   bonded TV has seen. If it differs from the board's, then on the next
   encrypted TV link, as soon as the TV enables `2A05` indications, the
   board indicates `0x0001..0xffff` once and saves the new version. This
   makes the TV throw away a cached copy of the v1 table.

Re-pairing test: TV search (§2.5), then BOOT. The board drops the TV bond
and advertises in pairing mode. Expected results:

- In the board log: `TV reads the Report Reference of report 0xf7 type 1`
  for all four reports, which never appeared with v1. Then
  `TV CCCD h=0x002f notify=1`, and possibly `sent Service Changed …`.
- In the TV vlog: `hid not ready` changes to `Register report ids`.

**Result of the v2 re-pair (15:16):** ❌ no change. The TV wrote the
Service Changed CCCD at h=`0x0003`, so it saw the new table. It then read
PnP ID, HID Info and the Report Map (13 reads at MTU 23). Its own MTU
exchange arrived 20 ms after the last Report Map read, about 2.1 s after
connecting, and nothing followed. The layout is therefore not the cause.

**Fix 3: the board starts the MTU exchange at connect (flashed ~15:19,
still in the firmware).**

Hypothesis: Bluedroid's GATT client keeps one pending command per
connection and drops a second one (`bta_gattc_enqueue` "already has a
pending command"). The TV's MTU request would take that slot just as the HID
host queues its first Report Reference read. The read would be lost and the
HID host would wait for it forever.

`tv.c` now calls `ble_gattc_exchange_mtu()` on `BLE_GAP_EVENT_CONNECT`. The
TV always connects with `itvl=8 latency=0 timeout=500`.

**Result: ❌ keys are still ignored.** Board log
`esp32-receiver/logs/monitor-20261102-151930.log`, fresh pairing:

```
39571  TV connected (pairing mode)
39591  MTU 247 agreed with the TV (our request)   <- before pairing, before any read
40171  PAIRED with TV 38:8c:ef:9c:18:7a
40781  TV reads PnP ID
41021  TV CCCD h=0x0003 indicate=1               (Service Changed)
42161  TV reads HID Info                         (~1.1 s gap before it: discovery)
42201  TV reads Report Map
42241  TV reads Report Map                       (2 reads at MTU 247; 13 at MTU 23 before)
42281  TV MTU 247                                <- the TV's OWN MTU exchange, again
       nothing more: no Report Reference read, no Protocol Mode write,
       no report CCCD write. Keys relayed from 47621 on are ignored.
```

**What we now know (board side):**

1. **The TV's MTU exchange is a fixed step of its HID setup, not a timer.**
   It comes 40 ms after the last Report Map read, whether that read took 13
   requests at MTU 23 or 2 at MTU 247, and even though the MTU was already
   247. It fits `cb_mtu_configuration_complete` in `hid-host-q.cpp`: the
   TV reads the Report Map, configures the MTU, and then should register
   the reports. The step after the MTU exchange is the one that never
   happens.
2. **The handle layout is not the cause.** With handles identical to the
   remote up to `0x0042` (layout v2), the behaviour is unchanged.
3. **The MTU timing is not the cause either.** An MTU of 247 before
   pairing, and a Report Map read 6× faster, change nothing.
4. **On a bonded reconnect (29001 in the same log), the TV starts the MTU
   exchange itself within 20 ms and reads nothing at all.** Our request
   then gets `rc=2` (`BLE_HS_EALREADY`, harmless). It never reads any
   characteristic, so it must use data cached during pairing.
5. **The board only logs ATT requests that reach our callbacks**: reads and
   writes of our characteristics, CCCD writes, and MTU. It does not log
   discovery (Read By Group Type, Read By Type, Find Information), and it
   does not log requests NimBLE rejects on its own: bad handle, permission
   or encryption errors. After the second MTU exchange the TV may send
   something the board answers with an error.

**Handover answered (2026-11-02 later, via `shell.sh` + full vlog pull + GATT-cache
forensics):**

The TV-side view of the failing session (kernel time 3920 in
`tmp/v2.txt`, `tmp/seg.txt` on this machine; grep `3920.1`–`3928.7`):

1. **The TV's HID host went READY and even created the uhid device.**
   `hid-host-q.cpp: setReadyStatus(285) > val 1`, `UHID_CREATE … this is
   SS smart controller [0c:4e:a0:61:b8:0a]`, `UHID_START`, `UHID_OPEN`,
   `BTA_GATTC_CFG_MTU_EVT status[0]`. So "hid not ready" in later sessions
   is a *downstream symptom*, not the root cause — the real failure is:
2. **`bta_hh_le_write_rpt: no matching report` (3920.414)** — the very first
   command the TV tries after HID open (`set_report` reportType=3, the
   `31 01 17` dev-info query as a Feature report) fails because **the TV's
   parsed report list is empty**. No `RFN not matched` for our reports; the
   stack simply has no report entries for this device.
3. Then `bta_hh_co_close` (3926.6) and `bta_hh_security_cmpl: encryption
   failed status=0x000e reason=0x0016` (connection terminated by the local
   host after the failure), followed by the eternal `-29` retry loop.

**The TV's GATT cache proves why the report list is empty.** The QStack
persists discovered attributes per peer in
`/opt/usr/apps/bluetooth-config/data/gatt_cache_<mac>` (pulled:
`tmp/gatt_cache_esp32` for the board, `tmp/gatt_cache_other` for the other
Smart Control). Byte-level diff of the `1812` sections:

| Entry | Real remote's cache | Board's cache |
|---|---|---|
| `2A4D` char (e.g. h `0x1e`↔`0x2f`) | `[perm][handle][uuid 2a4d+base]` — UUID stored | `[perm][handle][14 × 00]` — **UUID empty** |
| `2908` Report Reference | `[handle][uuid 2908+base][value 2 B]` | `[handle][14-byte garbage blob]` — **no UUID, no value** |

All four `2908` entries contain the *same* 14-byte blob
(`1a 00 39 00 37 0e 00 36 00 33 1a 00 32 00`) where the UUID + report-ID
value should be. The blob contains our own perm/handle bytes (`1a`, `0e`,
`37`, …) — i.e. the cache writer lost byte-sync and stored fragments of
neighbouring entries. Without valid `2908` values the stack cannot map
report IDs/types → the report list is empty → "no matching report" for
every command, CCCDs never written, keys `RFN not matched`.

The corrupted-entry pattern (misaligned garbage containing perm/handle
bytes) is characteristic of a **stale cache file being partially rewritten**:
the TV's cache for our MAC was first written during the **v1 layout era**
(different handles), and the v2 re-pair + Service Changed led to a rewrite
that interleaved with old bytes. Note the board changed its GATT table *twice*
(v1→v2 plus the `1800`/`1801` removal→re-add) while keeping the same MAC.

**ROOT CAUSE FOUND AND FIXED (2026-11-02 ~15:5x, not yet flashed): a
lifetime bug in `tv.c` — dangling UUID pointers.**

The `…:0c` test (fresh cache, no Service Changed) still failed, killing the
stale-cache theory. The fresh `gatt_cache_0c4ea061b80c` pulled off the TV is
**byte-identical** to the old one — the "corruption" is deterministic, i.e.
the TV faithfully caches what the board serves. Decoding the TV's btsnoop
ATT frames for the `…:0c` pairing (`tmp/seg4.txt`, kernel time 5575) gave the
proof:

```
TV: Find Information 0x0030–0x0031
<— 05 01 30 00 02 29        (handle 0x30, uuid 0x2902 CCCD — correct)
TV: Find Information 0x0031–0x0031
<— 05 02 31 00 1a 00 39 00 37 0e 00 36 00 33 1a 00 32 00
    ^^^^^^^ Find Info rsp, FORMAT 0x02 (128-bit UUID item!)
             handle 0x31 + 16 bytes of GARBAGE as the "UUID"
```

The same pattern for every report descriptor: the board answers Find
Information for the `0x2908`s (and Read By Type for the `2A4D` values) with
**format-0x02 items containing garbage bytes** — those garbage bytes are
exactly the "corrupted" cache entries. With no valid `2908` UUID+value the
TV builds an empty report list → `bta_hh_le_write_rpt: no matching report` →
everything downstream fails.

Why: in `tv_init()`, the report chars/descriptors were assigned at runtime
with `BLE_UUID16_DECLARE(0x2a4d)` / `(0x2908)`. That macro expands to a
pointer to a **compound literal on `tv_init()`'s stack**. NimBLE's GATT
registration (`ble_gatts_start()` → `ble_att_svr_register()`) runs at **host
sync — after `tv_init()` returned** — and stores the *pointer*. Every ATT
response built after that reads dead stack memory as the UUID (which is why
the bytes looked like our own perm/handle leftovers). The statically
initialized services were unaffected (compound literals in static
initializers get static storage).

**Fix (in `main/tv.c`):** two file-scope-lifetime objects,
`static ble_uuid16_t uuid_report_chr = BLE_UUID16_INIT(0x2a4d);` and
`uuid_report_ref = BLE_UUID16_INIT(0x2908);`, used by the runtime
assignments via `&…​.u`. This explains every earlier symptom across v1/v2/
new-MAC tests (all of them had this bug since the reports were first defined
at runtime).

Verify after flashing (no re-pair needed if the MAC is unchanged — the TV
will re-discover on reconnect since the cache's report entries are garbage;
safest: BOOT re-pair once): expect `TV reads the Report Reference of report
0x… type …` ×4 on the board, `TV CCCD h=0x002f notify=1`, and in the TV vlog
`bta_hh_le_write_rpt` succeeding + `Register report ids`.

**Flashed (esp32-receiver, after a full erase) with address
`0c4ea061b80d`**, so the TV starts from a fresh bond and cache. Test mode is
still on: no remote, Down tapped every second.

`check_layout()` now also looks up the HID `2A4D` characteristic and its
`2908` descriptor by UUID through `ble_gatts_find_chr()` /
`ble_gatts_find_dsc()`. These walk the same UUID pointers the ATT responses
are built from, so a dangling pointer is reported at boot as
`lookup by UUID … broken UUID pointers?`. The boot log passes:
`GATT layout matches the remote's handles`.

Re-pair: start the TV search (§2.5), then press BOOT. Because the board
skips the remote, it accepts BOOT right away.

**✅ CONFIRMED FIXED (16:1x, board log `latest.log`).** The TV completes HID
setup and **accepts the relayed keys**: the Down taps are confirmed by the
user on the TV. Board log:

```
29971  TV connected            29991  MTU 247 agreed (our request)
30521  PAIRED                  31081  TV reads PnP ID
31251  TV CCCD h=0x0003 indicate=1
31821  TV reads HID Info       31841/31861  TV reads Report Map
31881  TV reads the Report Reference of report 0xf7 type 1   <- first time ever
31901  … 0xf6 type 1    31921  … 0xfb type 3    31941  … 0xfb type 1
31961  TV CCCD h=0x002f notify=1 (0xF7)   31981  h=0x0033 (0xF6)   32001  h=0x003a (0xFB in)
32021  TV MTU 247
32081  TV->RC report 0xfb type 3 len=3: 310117   (dev-info query, then every 2 s)
```

The TV now sends `31 01 17` on the `0xFB` Feature char without a report-ID
byte, as §6.2 says. In test mode no remote is connected to answer, so the
TV retries every 2 s. With the remote connected, `voice_relay_cmd()`
forwards it and the remote's `33 …` reply goes back up the relay.

Next:
- Turn test mode off (`SRR_SKIP_REMOTE`, `SRR_TV_TEST_KEY`) and run the full
  remote → board → TV chain.
- Then run the voice trigger (§6.5, rooted TV) to capture the TV's real
  voice command sequence.

**Previous session results (kept for the record):**

- `CONFIG_SRR_OWN_ADDR` (Kconfig + `apply_own_addr()` in `main.c`,
  most-significant byte first; empty = burned-in MAC):
- `apply_own_addr()` in `main.c` runs in `app_main()` **before
  `nimble_port_init()`** and calls `esp_iface_mac_addr_set(mac,
  ESP_MAC_BT)`. The controller reads `ESP_MAC_BT` when it starts.
- The first draft called `ble_hs_id_set_pub()` in `on_sync()`. That only
  changes the host's copy of the address: the radio keeps the burned-in one,
  and pairing would compute its checks with the wrong address. It also
  doesn't compile on IDF 6.1, where the function returns `void`.
- **Flashed 2026-11-02 with `0c4ea061b80b`**, after a full flash erase, so
  every bond is gone.
- **Result with `…:0b` (board log `monitor-2026110215*`, ~16:0x): ❌ same
  stall.** The remote and the TV both re-paired. The TV read PnP ID, wrote
  the Service Changed CCCD, read HID Info, read the Report Map (2 reads),
  did its MTU exchange, then nothing. Keys were ignored.
  - **Confound:** because the flash was erased, the board's "layout seen by
    this TV" record was empty. The board therefore sent a **Service Changed
    indication (`0x0001..0xffff`) right after the TV subscribed**, between
    the PnP ID read and HID Info. So this was a fresh cache *plus* a Service
    Changed indication mid-setup.
  - No test so far has had a fresh cache without Service Changed.
  - If a Service Changed indication during HID setup is what corrupts the
    cache, the `…:0b` cache file on the TV should be corrupted in the same
    way. TV side: please diff `gatt_cache_0c4ea061b80b` against the real
    remote's.
- **Next: `…:0c` and no Service Changed (flashed after a full erase).** The
  Service Changed indication logic is removed from `tv.c`. The `2A05`
  characteristic stays in the table, but it is never indicated. A changed
  layout now means a new `SRR_OWN_ADDR`.
- **Result with `…:0c`, no Service Changed (board log
  `monitor-20261102-154632.log`, 15:47): ❌ same stall.** This is the first
  clean test: a brand-new address, so a fresh TV cache, and no Service
  Changed indication.

  ```
  41766  TV connected            41786  MTU 247 agreed (our request)
  42276  PAIRED                  42816  TV reads PnP ID
  43006  TV CCCD h=0x0003 indicate=1
  43676  TV reads HID Info       43696/43716  TV reads Report Map (2 reads)
  43756  TV MTU 247 (the TV's own exchange)
         nothing more; keys relayed from 85846 on are ignored
  ```

  **Neither a stale cache nor Service Changed is the cause.** The TV's
  accesses are identical in every test, and the board answers every
  request that reaches its callbacks. Step 5 of the test procedure applies:
  the answer is in the ATT exchange.
  - **TV side, first:** the TV's btsnoop and vlog for this session around
    15:47 (`0C:4E:A0:61:B8:0C`). Check the ATT frames between the Report
    Map read and about 1 s after the MTU exchange, plus the
    `bta_hh_le_*` / `bta_gattc_*` lines. Also diff the fresh
    `gatt_cache_0c4ea061b80c`: if its `2A4D`/`2908` entries are malformed
    again with a fresh file, the TV parses our discovery responses wrongly
    (e.g. Read By Type / Find Information response formats), and the cache
    was never stale.
  - **Board side, if needed:** log every incoming ATT request and our
    answer. NimBLE has no hook for this, so it needs a small patch in
    `ble_att_rx()`.
- **Test mode, flashed at 15:5x (app only, so the `…:0c` TV bond is
  kept).** It sends the TV a steady, known input without the remote:
  - `CONFIG_SRR_SKIP_REMOTE=y`: the board never scans for the remote. The
    relay advertises to the bonded TV right away.
  - `CONFIG_SRR_TV_TEST_KEY=0x61`: while the TV link is encrypted, the board
    replays a real **Down** tap on report `0xF7` (h=`0x002f`) every second.
    The tap is `61 00 00 00 00 00`, the same again 100 ms later, then the
    release `00 00 00 01 00 c2` 40 ms later. This is the most common
    sequence among 41 real Down taps. The board logs
    `test key 0x61 tap #N sent to the TV`.
  - Both settings are temporary entries in `sdkconfig.defaults`. Remove them
    and set both options back to `n`/`0x0` in the sdkconfig to restore
    normal operation.
  - **Update:** `SRR_TV_TEST_KEY` has since been removed from the firmware,
    together with its code and Kconfig entry, because the TV fix was
    confirmed with it. `SRR_SKIP_REMOTE=y` is still set.
  - Live: the TV reconnected at 5251 and the taps started at 6211. The TV
    still hasn't subscribed to `0xF7`, so on the TV side every tap should
    land as `RFN not matched` until HID setup completes. That makes it a
    once-per-second marker in the TV's btsnoop and vlog.
- The TV keys bond + `gatt_cache_<mac>` + smart-RC slot on our address, so a
  new MAC guarantees a **pristine cache file with no v1-era bytes** — the
  same guarantee as the live `rm gatt_cache_0c4ea061b80a` performed over `shell.sh`
  during the investigation (that file is currently deleted on the TV; a
  backup sits in `/tmp/gatt_cache_backup` on the TV).
- `sdkconfig.defaults` carries a commented example (burned-in + 1).
  Changing the address invalidates *all* bonds — re-pair the remote
  (Back+Play/Pause) and the TV (BOOT + TV search §2.5) once after flashing.

**Test procedure after flashing (any address change or a live cache
delete):**

1. Re-pair remote → board (hold Back+Play/Pause on the remote).
2. Re-pair TV → board: TV search (Flipper IR / §2.5) + BOOT.
3. Success criteria, board log: `TV reads the Report Reference of report
   0x… type …` ×4 (has never appeared), then `TV CCCD h=0x002f notify=1`.
4. Success criteria, TV vlog (one-shot command channel, `vlogutil …`):
   `bta_hh_le_write_rpt` succeeding (no "no matching report"),
   `Register report ids`, and no `hid not ready` loop.
5. If it STILL fails with a fresh cache: the stale-cache theory is dead —
   capture the ATT exchange itself (NimBLE debug log or `ble_att_rx` hook) and
   compare Find-Information / Read-By-Type responses with the real remote's
   (the `9e5d1e47…` service at `0x0043` instead of `0xff00` is the next
   suspect: some stacks treat handles >0x0042 oddly, or the TV's discovery
   range ends at 0x0040).

Also observed live: `interop_mtk_match_name: Device Smart Control 2016 is a
match` — the TV applies an MTK interop quirk by NAME, another reason the
board must keep presenting exactly the real remote's name.

**Handover: next steps (for the TV-side agent):**

- **First, get the TV's view of 42281–42400 in this session.** TV vlog
  around the second MTU exchange: `bta_hh_le_*` /
  `bta_gattc_*` / `hid-host-q` / `oal-hid-host` lines, any `ERROR`, and
  `RFN` / `registered_report`. Also the btsnoop ATT frames after the
  Report Map read:
  - Does the TV send a request that gets an ATT error response?
  - Does it send nothing at all?
  - Did its discovery find the `0x2908` descriptors? Check the Find
    Information responses for handles `0x0030`/`0x0031`, `0x0034`/`0x0035`,
    `0x0038` and `0x003b`/`0x003c`.
- **Compare with a real remote ⇄ TV HID setup**, from btsnoop or vlog of
  the real remote pairing to the TV. Look for the step that comes after
  the Report Map read and the MTU exchange; that is the step our board
  never reaches.
- **Board-side follow-ups**, if the TV side points there:
  - Log every incoming ATT request. Raise NimBLE's log level to DEBUG, or
    hook `ble_att_rx`. This would show the exact last request and any
    error the board answers it with.
  - Remaining layout difference: the `9e5d1e47…` vendor service is at
    `0x0043..0x0048` instead of `0xff00..0xffff`, because NimBLE assigns
    handles sequentially.

The board firmware is in this state now: layout v2, Service Changed, MTU
exchange at connect. Its bond with TV `38:8c:ef:9c:18:7a` is fresh, from
39571–40171 in the log above.

**ESP32-C3 controller pitfalls found on the way:**

- `BLE assert sch_arb.c 517` + interrupt watchdog reset when a central (the
  TV) connects to the board's advert while the board scans, active or
  passive, 100 % or 50 % duty. Workaround in the firmware: advertise to the
  TV only while the remote is connected, which is when the board doesn't
  scan.
- Extended advertising reports `BLE_GAP_EVENT_ADV_COMPLETE` with reason 0
  just before `BLE_GAP_EVENT_CONNECT`: only treat `BLE_HS_ETIMEOUT` as the
  end of pairing mode.
- After the remote dropped the board's link and went back to pairing mode,
  the controller reported its adverts with address `00:00:00:00:00:00`;
  the firmware connects to the bonded identity address instead.
- `LE Add Device To Resolving List` fails (`0x12`) for the TV's restored
  IRK ("failed to configure restored IRK"); harmless, the TV uses its
  public address.
- Once, the remote's bond disappeared from the store after the TV paired,
  with no delete logged; not reproduced since. The firmware now lists the
  bonds after each TV pairing.

#### BLE sniffer: ground truth for both directions

An nRF52 Sniffer / Ellisys capture of a real TV⇄remote voice session is the
quickest way to close the remaining ❓ items (exact extend/stop bytes, frame
layout, group-0x34 traffic). Wireshark's `bluetooth_hid` dissector + vendor
report filtering does most of the work.

#### What to record for follow-up analysis

Timestamped raw frames of ALL reports (key events, telemetry, session
control), the exact stop/extend bytes observed from the TV, and the `0xFB`
notification cadence (verify ~42/s). All RE evidence for this section is in
`REMOTE.md` §9.

---

## 7. Firmware update (OTA) — ✅ CONFIRMED flow

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
file/bluetooth/bt_smart_rc_version          # remote firmware version
file/bluetooth/bt_smart_rc_device_type       # remote hardware type
file/bluetooth/bt_smart_rc_capability        # capability flags
file/bluetooth/bt_smart_rc_language_version
file/bluetooth/bt_last_connected_smart_rc    # remote BD address (string)
file/bluetooth/bt_last_connected_smartrc_name
file/bluetooth/bt_headless_info_smart_rc1    # bonded remote slots (2 supported)
file/bluetooth/bt_headless_info_smart_rc2
memory/bluetooth/latency_status             # latency-set state machine
db/btapp/lpm_mbr                             # LPM MBR mode state
rc_fw_upgrade                                # staged remote firmware path (OTA trigger)
```

### 8.2 Daemons and interfaces

- `bt-service` (D-Bus: `org.projectx.bt`, `org.projectx.bt_product`) — owns the
  BLE connection, HID report registration, voice, latency control, OTA push.
  (Full D-Bus method catalog: ❓ enumerate with e.g. `busctl` on the TV.)
- `bt.updatemanager` (D-Bus: `com.samsung.tv.bt.updatemanager`, methods `HELLO`,
  `CHIP`, `SCFW`, `ARK`, `ARK_USB`, `EXIT`) — runs the remote-chip/SmartRC/Ark
  update state machines. `SCFW` triggers the Smart Remote firmware update.
- Files it reads: `/opt/data/ci_fw_update/smartrc_fw/*.{hex,sign}` and
  `/usr/apps/org.tizen.btapp/shared/res/fwdir/SC_BT_Public_key.pem`.

---

## 9. Open items (things a capturing agent should record to complete this doc)

| # | Item | How to get it | Status |
|---|---|---|---|
| 1 | Full GATT handle table (char ↔ report ID mapping) | Dump on first connect | ✅ §3, §4.0 (0xA001/0xA002 purpose still ❓) |
| 2 | HID Report Map (authoritative report ID catalog) | Read `0x2A4B` after pairing | ✅ decoded from fw (§4.1) |
| 3 | Key code ↔ button names | `samsung-remote/esp32-receiver/keymap.sh` | ◐ all keys on `0xF7` (§5.4); Power `0xD2`, MIC `0xA0`, rest unnamed |
| 4 | Why the remote sent no audio after a correct start | — | ✅ it did: audio is on `0xF6`, not `0xFB` (§6.3). Still ❓: `31 24 xx`, `34 31 07 00 00`, the `60 03 00 00 00` reply |
| 5 | Voice codec parameters on the link | Captured `0xF6` stream | ✅ mSBC, 16 kHz mono; report = `<seq>` + 3 × 57-byte frames (22.5 ms), 44.4/s (§6.4). Stereo/far-field layouts ❓ |
| 6 | DFU/OTA service + opcodes (vendor `0xA001/0xA002`?) | Dump GATT (item 1) + sniff an OTA session | ❓ |
| 7 | D-Bus method catalog of `org.projectx.bt` | `busctl tree org.projectx.bt` on the rooted TV | ❓ |
| 8 | Whether the remote accepts a second concurrent bond | Pair to Linux, then re-pair to TV and observe | ❓ |
| 9 | Which transport a given remote uses (Gen A eSCO vs Gen B HID-0xFB) | Linux GATT dump — see §6.1.1 | ◐ criteria documented |
| 10 | ESP32 relay: TV bonds to the board (trigger per §2.5), Dev Info answered, voice session relayed end-to-end | §6.5 MITM relay status | ◐ **root cause FIXED and verified live**: `BLE_UUID16_DECLARE` inside `tv_init()` created stack compound literals; NimBLE registers at host-sync → dangling UUID pointers → garbage Find-Info/Read-By-Type responses → TV report list empty (`no matching report`). Fix: static `ble_uuid16_t` objects. ✅ The TV now reads all four Report References, subscribes, accepts relayed keys and sends `31 01 17`. ✅ Full chain remote → board → TV: keys work, TV commands and replies relayed, TV-started voice streams. ❌ The TV's own capture (tvctl `record_remote_mic.sh`) still got 0 bytes through the relay, and the TV link dropped once while streaming (`0x222`). Default now: voice stays on the board (`SRR_VOICE_LOCAL`) |

## 10. Suggested capture setup

**TV-side commands and file transfer: use `./shell.sh`** (repo root). It is
this lab's private channel to the TV — **never read inside it or the
tooling it calls, never modify or commit it**; if it misbehaves, ask the
human instead of debugging it. If it reports the channel is down, run
`python3 tvctl/tvctl.py activate <tv-ip>` once and retry.

**Primary: ESP32 (NimBLE) — see §6.5 for the full recipe and the three pitfalls**
(bond/encrypt → MTU 247 → while MIC is held, write `31 04` to the
**Feature** 0xFB Report characteristic over the bonded link; audio on `0xF6`):

```
# 1) power off nearby Samsung TVs
# 2) hold Back+Play/Pause ≥ 3 s (LED blinks → pairing mode; drops the TV bond)
# 3) ESP32 (NimBLE central, IO cap NoInputNoOutput): scan → connect → bond
# 4) MTU exchange ≥ 247
# 5) discover 0x1812; read every 0x2908 Report Reference descriptor;
#    subscribe (CCCD) on the (0xF6, Input) and (0xFB, Input) Report chars
# 6) press and hold MIC, then write 31 04 (and 31 03 10) to the
#    (0xFB, Feature) Report char before releasing it; 31 02 on release
# 7) log every notification; press each key once for the key→report map
```

Alternative capture paths:
- **Linux (bluez)** — disable `hog`/`input` plugins first (§6.5); bleak/btgatt-client then see everything.
- **Rooted TV** — `python3 tools/voice_poc.py <tv-ip> --bt` (proven; works for both voice generations).
- **BLE sniffer** — nRF Sniffer on a real TV voice session (ground truth, both directions).
- **macOS — blocked**: CoreBluetooth hides the HID service (§6.5); don't retry there.

Raw RE evidence for every claim above: see `REMOTE.md` (§4, §9).
