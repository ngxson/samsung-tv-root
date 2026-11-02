# Samsung Smart Remote — Bluetooth Protocol & Firmware Research

> **Protocol reference for implementers:** `REMOTE_PROTO.md` contains the
> clean, RE-free protocol spec (pairing, key press, voice, OTA, host-side APIs)
> written for an agent that needs to *use* the protocol. This file
> (`REMOTE.md`) holds the reverse-engineering evidence: binaries, addresses,
> disassembly, and analysis notes.

Findings from the rooted TV firmware dump (`dist/dump/rootfs`) and static analysis of
the remote's own firmware images. All analysis done on this machine, no TV required.

- TV model family: 2025 Samsung Tizen TV (dump used throughout this repo)
- Remote: Samsung Smart Remote (solar/USB-C rechargeable, BLE voice remote,
  BN59-013xx family)
- Remote SoC: **Realtek RTL8763B-family** (dual-mode BT audio SoC)
- Remote firmware drop: **Y25** (2025), version `1.0.185.0`

---

## 1. TV side: who does what

| Component | Role |
|---|---|
| `/usr/bin/bt-service` (bluetooth-frwk-0.2.147_tv) | BT daemon. D-Bus names `org.projectx.bt` / `org.projectx.bt_product` (`etc/dbus-1/system.d/bluetooth-frwk-service.conf`) |
| `/usr/bin/bt.updatemanager` (org.tizen.btapp-25.07.16.0) | Remote OTA orchestration: candidate FW discovery, version compare, signature check, push trigger |
| `prd/usr/lib/libbluetooth-impl-sec.so.0.1` (bluetooth-impl-sec-26.06.23) | Samsung backend: `hid_*`, `gattc_*`, `ota_update_firmware` implementations |
| `prd/usr/lib/libbt-oal.so.0.1` | OAL with `ScOtaManager` (`oal-sc-ota-mgr.cpp`) — the remote OTA state machine |
| `prd/usr/lib/libbluetooth_mtk.default.so` / `_rtk.default.so` | TV's own BT controller stack (MTK/Realtek) |

### Remote state in TV vconf keys

```
file/bluetooth/bt_smart_rc_battery
file/bluetooth/bt_smart_rc_version           <- remote fw version
file/bluetooth/bt_smart_rc_capability
file/bluetooth/bt_smart_rc_device_type
file/bluetooth/bt_smart_rc_language_version
file/bluetooth/is_smart_rc_paired
file/bluetooth/bt_last_connected_smart_rc
file/bluetooth/bt_headless_info_smart_rc1/2  <- headless (PIN-less) pairing info
memory/bluetooth/bt_smart_rc_connected
memory/bluetooth/latency_status
db/btapp/lpm_mbr
```

---

## 2. How the remote link works

- The remote is a **BLE device** doing **HID-over-GATT (HOGP)** — not classic BT HID.
- bt-service classifies devices at connect: "Device is BLE RC" / "Its Wearable remote
  controller" / mobile remote / AI speaker remote.
- Input arrives via **vendor report IDs registered with the HID stack**
  (`hid_register_reports_impl(uint8_t*, len, cb)`):
  - Key input reports (buttons)
  - Touch input reports (touchpad/pointer)
  - Sensor input reports (motion)
  - Voice reports — mic data streamed into the TV voice pipeline (captured in this
    repo as `voice-poc-recording.wav`)
  - **Dev Info report** — battery, firmware version, device type, capability
  - **MBR_RETURN_REPORT** — vendor low-power/latency control channel
    (`adapter_lpm_mbr_mode`, LPM MBR enable/disable)
- Before voice capture or firmware update the TV **reprograms BLE connection
  parameters**: slave latency → 0, then reverts to power-saving latency afterwards.
- Remote type ↔ firmware type matching happens at runtime: bt.updatemanager logs
  "FW type %s remote type %s" — the remote's type comes from its Dev Info report.

---

## 3. Remote OTA: yes, two transports

### 3.1 HID transport (`hid_smartrc_fw_update`)

For older "Normal RC" remotes. Firmware pushed over the HID profile via **SET_REPORT**
to vendor report IDs. Log: "FW update for Normal RC: result [%d]".

### 3.2 GATT OTA transport (`gattc_ota_fw_update` → `ScOtaManager`)

For BLE remotes. The TV acts as a **GATT client** connecting to the remote's
OTA/DFU GATT server (Realtek DFU — see §5):

```
@@FW path allocated for BLE RC fw update
Set Latency befor FW update..
Latency Set successful...proceed with FW update..
(MTU exchange)
Send OTA FW Chunk of %d bytes
!!!!  OTA FW DL Progress [%d%%]  !!!!
OTA FW update for BLE RC in progress now...
!!!OTA FW UPDATE COMPLETED [status : %d]!!!
BLE RC Disconnected, after FW update, remove timer..
```

Flow reconstructed from strings:

1. TV update system downloads remote firmware and writes the **file path into vconf
   key `rc_fw_upgrade`**
2. On next remote connect, bt-service checks it ("Check if FW update requested for
   BLE RC")
3. Connection latency → 0, wait for ACK
4. GATT connect as client → MTU exchange → chunked writes, with progress + timeout
   timer ("ota fw update timer expired ... revert latency")
5. Remote reboots into new firmware, reconnects, reports new version via Dev Info
   report → stored in `file/bluetooth/bt_smart_rc_version`

The same OTA framework is reused for other BT accessories: the **Rotator**
(motorized swivel stand — BT + MCU firmware, rotation count) and the **Ark Switch**
(`bt-service-hidhost-product-rotator.c`).

Note: distinct from the TV's **own BT controller chip** firmware
(`FirmwareManagerMtk`/`FirmwareManagerRtk`, `/proc/driver/btusb-fw-upgrade`).

### 3.3 Where the firmware comes from (no TV needed)

The remote firmware ships **inside the TV firmware image**:

```
TV rootfs: /opt/data/ci_fw_update/smartrc_fw/   <- 10 .hex + .sign files (Y25 drop)
            /usr/apps/org.tizen.btapp/shared/res/fwdir/SC_BT_Public_key.pem
```

Copied to this repo at **`remote-fw/`**:

```
remote-fw/
├── SC_BT_Public_key.pem                      <- RSA-2048 verification key
└── smartrc_fw/
    ├── SC_BT_Y25_{20,E1,E2,E4,E6,E7,E9,EA,F3,F5}592200.hex   (Intel HEX)
    └── ...matching .sign files               (base64 256-byte RSA-2048 sig)
```

To fetch *newer* remote firmware without a TV: download the TV firmware package from
Samsung (USB-upgrade zip from samsung.com or the OTA endpoints already mapped in
`REPORT.md`: `osb-v2.samsungqbe.com` / `samsungotn.net`), decrypt with this repo's
`swu/` toolkit, extract `smartrc_fw`.

### 3.4 Signature chain (verified locally)

- `.sign` = base64 of a raw 256-byte **RSA-2048 signature**
- Signed object: the raw `.hex` file bytes, digest **SHA-256**
- bt.updatemanager verifies with OpenSSL `EVP_DigestVerify` against
  `SC_BT_Public_key.pem` before pushing ("Signature verification OK" /
  `SignedCheckFailed`)
- Reproduced on this machine:

```bash
base64 -d -i SC_BT_Y25_20592200.sign -o /tmp/sig256.bin
openssl dgst -sha256 -verify SC_BT_Public_key.pem \
        -signature /tmp/sig256.bin SC_BT_Y25_20592200.hex
# Verified OK
```

Integrity/authenticity only — no confidentiality (see §4).

---

## 4. Remote firmware structure (disassembled)

### 4.1 Not encrypted

- Entropy: avg **6.66 bits/byte** across 535 KB (encrypted would be ~7.99 uniform);
  only 6 of 130 4KB blocks exceed 7.5 bits (data regions, not code)
- Entry point disassembles cleanly as ARM Cortex-M **Thumb**
- Readable strings throughout (`RTKBeeFirmwareRev`, `sm_sc_send_dhkey_check`, …)

### 4.2 Chip identification: Realtek RTL8763B-family

Evidence:
- `RTKBee{SerialNum,HardwareRev,FirmwareRev,SoftwareRev,IEEEDatalist}` config keys,
  "Realtek BT", "Model Nbr 0.9"
- Image magic **`0x8721BEE2`** ("Bee" — Realtek's Bee SDK)
- `I2S0_RX/TX` (digital mic), `usbchg_full_charge_debounce_timer` (USB-C charging —
  RTL8763B datasheet: "USB BC1.2 battery charging", dual analog/digital MIC)
- `secure_boot version: %d.%d.%d` strings
- Firmware rev strings: `1.0.185.0`, `1.0.62.1`

### 4.3 Memory map (recovered from pointer analysis)

| Region | Address | Evidence |
|---|---|---|
| Mask ROM (~512KB) | `~0x228D0000–0x22F42000` | many negative-offset BLs (memcpy, print, crypto, flash-write live in ROM) |
| **Flash XIP** | **`0x23100000`** | 1162 pool words `0x2310xxxx`; base built arithmetically at runtime (`movs rX,#0x23; lsls #20…`) |
| SRAM | `0x08800000` + low RAM `0x0020xxxx` | data/bss pointers |
| Retention RAM | `0x1B7F0000` | initial SP = `0x1B7F000C` |
| Peripherals | `0x40000000+` | 84 refs to `0x40000000` block (UART0 — log/MP UART), `0x40014000`, `0x40026000`, … |

ABI quirk (important for Ghidra): **no absolute pointers to strings** — the ROM
print API takes flash *offsets* + module IDs; flash base is OR'd in at runtime.
Load `.bin` at `0x23100000`, little-endian, Thumb; strings resolve via offset-pools.

### 4.4 Image layout (all 10 variants)

Converted flat binaries: `remote-fw/bin/SC_BT_Y25_*.bin`

```
0x00000   Boot manifest: SP=0x1B7F000C, reset=0x2790 (only 2 valid vectors —
          the rest is ROM's job)
0x00400   Secondary header: entry 0x2793 + 16-byte hash + bank size fields
          (0x832000 / 0x832400)
0x01000   ── BANK 0 code (Bee SDK + BLE stack + Samsung app) ──
0x03B00   syscfg table: "Realtek BT", "Model Nbr 0.9", RTKBee* keys, "BTMAC"
0x0A500   MP TEST module (see §6)
0x16A30   secure_boot (see §4.5)
0x12960   mp_test_set_fast_pair_info (data)
0x2BF00   DFU/OTA engine (see §4.6)
0x3C800   DFU opcodes handler: "DFU_OPCODE_START_DFU: dfu_update fail=%d",
          "New Image too large!", "Temp bank too small!",
          magic check 0x8721BEE2 → "wrong signature! Read %8X != Requried %8X"
0x03DCC   remote TYPE ID byte (0x20/E1/E2/E4/E6/E7/E9/EA/05 — matches filenames!)
0x03FC4-5 type sub-fields (e.g. 0x05 0x02 vs 0x15 0x0E)
0x123EE   per-type code patch (0x2B / 0x00 / 0x73 / 0x01 — likely key-matrix/GPIO)
0x3E000   13.6 KB high-entropy blob (crypto material/keys — identical across all
          10 types)
0x4151C   ── BANK 1: same image again (factory ships both OTA banks populated) ──
0x7F000   Bank 1's high-entropy blob
0x829F8   end (~535 KB total; F3/F5 variants slightly larger)
```

### 4.5 Secure boot (bank0 @ 0x16acc / 0x16b02)

```asm
0x16ad2: mov r2, sp            ; copy 0x1E0 bytes of "FT data" to stack
0x16ad6: r1 = 0x1E0            ; 480-byte FT blob
0x16ae8: bl ROM_verify         ; ROM: verify 0x20-byte signature over it
0x16af6: → print "ERROR: NO Valid FT Data!"
; second blob: 0x60-byte "OEM config" + 0x20-byte signature
;            → "oem config valid %d, at 0x%x"
```

Boot chain: read signed **FT (factory) data blob** + signed **OEM config blob** →
verify each via ROM crypto → print `secure_boot version: %d.%d.%d` → proceed only
on success. eFuse holds at least xtal calibration
(`[mp_extension_write_xtal] write xtal to efuse`) and secure-boot material.

### 4.6 DFU/OTA engine

- Dual-bank layout: DFU writes the inactive bank, then swaps
  ("New Image too large! total_length=%d, temp_bank_size=%d",
  "Error DFU_TEMP_BUFFER_SIZE > 4KB")
- Incoming images must carry magic **`0x8721BEE2`** in the header, else
  "wrong signature! Read %8X != Requried %8X"
- Completion → `ota_complete_reboot_timer` → reboot into new bank
- The TV's `gattc_ota_fw_update` is the client for this server. (An earlier note
  guessed `0xFE59` was the DFU service UUID; the `59 fe` byte hits in both
  binaries turned out to be **code bytes, not UUIDs** — treat the DFU service
  UUID as still unknown. A vendor service `0xA001` + characteristic `0xA002`
  (notify) exists in the remote's GATT table and is the prime DFU/OTA candidate —
  see §9.3.)

### 4.7 The 10 "types" are one firmware + data patches

Comparing e.g. `20592200` vs `E1592200`: only 4 meaningful byte differences
(type ID `0x3DCC`, sub-fields `0x3FC4/5`, one code byte at `0x123EE`). Banks differ
by only 6 of 131 4KB blocks. One firmware, ten data patches.

---

## 5. Security implications

- The remote firmware accepts images over BLE from the TV; the TV gates on RSA-2048
  (public key known). The **open question** is whether the remote-side ROM/DFU
  verifies the image cryptographically (secure boot is present, so likely yes) —
  a rooted TV pushing an unsigned image would then fail at the remote.
- The `.hex` images are plaintext, symbol-rich full flash images of a BLE HID
  device paired to millions of TVs — fully Ghidra-ready (base `0x23100000`, Thumb).
- TV-side `hid_send_data_impl` lets the TV send arbitrary vendor report IDs
  (voice-trigger, latency, MBR control) to the remote.

---

## 6. UART / debug access (hardware)

**Question raised: is the log UART disabled via eFuse? UART pads show no output.**

### Findings

1. **The production app has no log output compiled in.** No log-module strings, no
   baud-rate strings in the app image; all diagnostics (secure boot, MP test, DFU)
   print via **ROM** calls. In normal boot the app never prints — UART silence after
   the ROM banner phase is expected production behavior, not necessarily eFuse.
2. **eFuse is used** for xtal calibration and secure-boot-related material; the
   signed "FT data" blob (verified at boot) is the structure Realtek uses for
   factory config, which can carry debug/UART-disable bits. An FT/eFuse-mediated
   ROM-log-disable is plausible for the boot banner phase.
3. **The UART is alive in MP (mass production) test mode — triggered by a hardware
   pin, not software:**

```
#[mp_test_check_trig_gpio_status] waiting MP Test pin release
[mp_test_check_trig_gpio_status] Switch to HCI UART test mode!
[mp_test_check_trig_gpio_status] Switch to Data UART test mode!
[mp_test_check_trig_gpio_status] Switch to Single Tone test mode!
[uart_test_enter_fast_pair_mode] Switch to Fast Pair %d test mode!
[uart_test_reboot_device] reboot device!
[uart_test_enter_hci_mode] Switch to HCI UART test mode!
```

The firmware samples a **dedicated MP test pin at reset**. While held at power-up,
UART0 becomes a command channel for Realtek's **Bee MP Tool / MPCli** (published at
realmcu.com, Windows/Linux/macOS): read/write flash, read efuse (BTMAC, xtal),
download images, RF tests. Samsung's factory needs this to flash remotes, so the
UART is definitely bonded out.

### Practical plan

- Hold the **MP test pin** at reset (look for an unpopulated pad labeled
  `MP`/`TEST`/`TP` near the SoC), watch UART0 @ 115200 for the
  `waiting MP Test pin release` banner.
- Logic analyzer at battery-insert/reset: brief banner then silence → ROM log on,
  app logging off; nothing at all → wrong pad or ROM log disabled via FT/eFuse.
- If UART stays dead: secure boot implies SWD is likely eFuse-disabled too; the
  intended backdoor is MP-test UART, and the **BLE DFU path is fully mapped**
  (§4.6) — a standalone OTA client can be reimplemented from these findings.

### TP_DSCL / TP_DSDA are NOT SWD (and not the Realtek)

Checked against the RTL8763B datasheet pin tables (QFN40/48/68): the Realtek SoC
has **no pins named DSCL/DSDA**. Its pads are RF, XI/XO, P0_x/P1_x/P2_x GPIOs,
audio codec, power, SPI-flash, USB, and `HW_RST#`. The Cortex-M4F's SWD port
exists but is **multiplexed onto GPIO pins** (pinmux table needed from the HDK to
know which P0_x carry SWCLK/SWDIO) — and secure boot likely disables it anyway.

`DSCL`/`DSDA` (Debug Serial CLock / Debug Serial DAta) is the standard naming for
the **proprietary 2-wire ICP programming/debug interface of companion 8-bit MCUs**
(ABOV Semiconductor style — Korean MCU vendor common in Samsung appliances; see
ABOV E-OCD II tooling). It is not I2C despite the name, and not SWD.

Conclusion: **TP_DSCL/TP_DSDA on the remote PCB almost certainly belong to a
companion 8-bit MCU** (touch/keyscan or power/charging controller), provided for
factory firmware programming of *that* chip. They will never show UART traffic
and are useless for reaching the Realtek SoC.

How to confirm on hardware:
- Trace continuity from TP_DSCL/TP_DSDA — they should land on a small
  SOP/DFN/SOT-package IC, not the main SoC.
- That IC can be programmed/consulted with the vendor's ICP tools if you ever
  want its firmware (minor attack surface: key matrix / touch handling).

### Getting the Realtek UART up: the P2_0 strap

From the RTL8763B datasheet GPIO table:

> `P2_0` … **"HCI mode selection: H = APP mode, L = HCI mode"**

P2_0 is a **strapping pin**: pulled low at reset, the SoC enters HCI mode and the
UART comes up for HCI/MP traffic (exactly the "HCI UART test mode" from the fw
strings). On the board, find the pad connected to P2_0 (continuity from the SoC
side, or try candidate pads at reset) and strap it low while releasing reset.
This, plus the MP test pin, are the two hardware paths into the chip.

---

## 7. Tools & artifacts in this repo

- `remote-fw/smartrc_fw/*.hex` + `*.sign` + `SC_BT_Public_key.pem` — copied from
  the TV dump
- `remote-fw/bin/*.bin` — flat converted images (Ghidra-ready)
- Key TV binaries referenced: `dist/dump/rootfs/usr/bin/bt-service`,
  `usr/bin/bt.updatemanager`,
  `prd/usr/lib/libbluetooth-impl-sec.so.0.1`, `prd/usr/lib/libbt-oal.so.0.1`

## 8. Open items

- [ ] Extract MP test pin GPIO number from boot code
- [ ] Full DFU state machine (opcodes/characteristics) for a standalone OTA client
- [ ] Confirm remote-side signature verification of OTA images (ROM analysis)
- [ ] Identify high-entropy blob at 0x3E000 (likely keys; identical across types)
- [ ] Ghidra loader script (base 0x23100000, offset-pool string fixups)
- [ ] **Why no audio after a correct start** (`34 31 1A 00 00` + `31 04`
      inside the MIC window, both ATT-acked): the mic capture/encode path runs
      in the SoC's mask ROM — not in the flash image. Next steps:
      `dlogutil BT_FRWK_SVC` on the rooted TV during a real MIC press, or an
      nRF-sniffed TV voice session (REMOTE_PROTO.md §9 item 4).
- [ ] Multi-key encoding in the `0xF7` key report (release checksum mismatch
      with two keys held) — REMOTE_PROTO.md §5.4
- [ ] Purpose of vendor chars `0xA001`/`0xA002` (one all-zero 16-byte
      notification on subscribe; write handler wants ≥16-byte payloads) and the
      `9e5d1e47-…` service (Realtek DFU candidate)

---

## 9. Voice / mic & vendor-report RE evidence (source of REMOTE_PROTO.md §4/§6)

All protocol-facing conclusions live in `REMOTE_PROTO.md`; this section records
*where the bytes came from*.

### 9.1 HID Report Map + GATT table in the remote image

- Report Map descriptor blob: bank0 **`0x11EE4`–`0x12000`** (bank1 duplicate at
  `0x53380`+). Decodes to report IDs `0x01` (boot keyboard), `0x02` (rel mouse),
  `0x07`/`0x08` (vendor FF01/usage 0x14, 20 B, Input/Output), `0xF6` (consumer,
  180 B), `0xF7`/`0xF8` (vendor FF00 usage 0x2F, 6 B), `0xF9` (18 B), `0xFB`
  (**192 B + 11 bits Input** = voice audio, and a **Feature** twin = command
  channel).
- GATT attribute table: bank0 **`0x12004`–`0x12380`**: HID service `0x1812`,
  Info `0x2A4A`, Control Point `0x2A4C`, Protocol Mode `0x2A4E`, Report Map
  `0x2A4B`, **8× Report `0x2A4D`** with Report Reference `0x2908` descriptors
  (`0xF7` Input, `0xF6` Input, `0xFB` Feature, `0xFB` Input) and CCCDs `0x2902`;
  then vendor service **`0xA001`** + characteristic **`0xA002`** with CCCD.
- Device name string in fw: `"Smart Control 2016"` (at `0x1236E`); key-map
  consumer-usage tables at `0x123D8`/`0x123F8`.

### 9.2 TV side — bt-service (usr/bin/bt-service, ARM Thumb, non-PIE)

- Voice command sender function **`0x3029C`** (logs "Sending Voice command:
  0x%2x", "Voice Command [0x%.2X] length [%d]"). Callers: `0x30DE0`, `0x30F68`
  (D-Bus handlers for the start/stop/extend product requests), `0x31544`/`0x315D0`
  (voice-key event paths).
- Remote-report dispatcher at **`0x3031A`**: `ldrb r3,[r4]` vs `0x1F–0x34`, tbb
  jump table at `0x3032C` ("Unknown Sub Report ID", "receive report id [0x%2x]
  for AI SPK", "Not voice command: 0x%.2X").
- **Command builder** at **`0x305E6`** (file offset; vaddr `0x405E6`):
  `movw r2,#0x31FB; strh r2,[r7,#0x14]` → buffer `FB 31`, then
  `strb r3,[r7,#0x16]` (event byte); the event==1 path at `0x306B2` appends
  `01 17` → **`FB 31 01 17`**. ⚠️ **Corrected 2026-11-02 (first pass was
  wrong):** cmd 1 with the `01 17` suffix is the **device-info query**
  (`__bt_service_fetch_hid_device_info`), not the voice start — live ESP32
  testing answered it with the Dev Info report (`33 17 00 03 00 00 02 00 02 31
  a3 00 00 00 ea 59 22 00 11`) and no audio. The voice start is **cmd 4**
  (from the voice-key handler), extend **3**, stop **2**, stereo start **6**.
  See §9.6 for the verified cross-check. Context strings: "start_sending_voice
  command" (`0x9F825`), "Sending start voice command from SC wake up Always
  must be first" (`0xA6889`), "Set voice recognition mode [%d]" (`0xA00C1`),
  "Send stereo voice start command" (dual-mic remotes).
- **Wire framing correction** (live): the GATT value carries **no leading
  report-ID byte** — the TV's HID layer takes `FB` as the report ID from the
  SET_REPORT descriptor (`__bt_smart_rc_set_report` hex-encodes and sends it as
  a Feature report), and the value on the link is `31 …` / `34 …`. Writing
  `FB 31 …` over GATT is ignored by the remote.
- **Latency control is an HID command, not LL**: `34 31 1A 00 00` /
  `34 31 1B 00 00` (no-latency on/off), matching `__bt_set_ble_conn_no_latency`.
- **Link audio codec = ADPCM**: bt-service contains `AdpcmDecode`,
  `InitAdpcmDecoder`, `__bt_decode_adpcm_data`, `adpcmIndex` (`0x3B24`–`0x3B30`);
  decoded output delivered as 16 kHz mono s16le through the voice-data socket
  (`__bluetooth_read_voice_data` in libbluetooth-api) → CAPI callback
  `{int16 length; void* audio_buf}`.

### 9.3 TV side — libbluetooth-api.so.1.0.0 (load base 0x472A0000)

- `bluetooth_product_hid_send_mbr_cmd` @ `0x472BE630`: allocates len+1,
  `movs r3,#0x20; strb r3,[r0],#1` + memcpy → report **`0x20`** = MBR command
  (`20 <len> <data>`); twin `send_mbr_data` @ `0x472BE3D0`. Remote answers with
  MBR_RETURN_REPORT ("MBR_RETURN_REPORT received" in bt-service, exposed as
  `bt_hid_set_mbr_return_receive_cb`, struct `{length, mbr_return}`).
- `bluetooth_product_hid_send_rc_haptic_data` @ `0x472BF488`: `movs r4,#0x23;
  strb r4,[r7,#0x14]` → report **`0x23`**, payload ≤7 B, len+1 header (`cmp r4,#7`).
- Voice path wrappers (D-Bus → bt-service): start `0x472BF084`, extend
  `0x472BEEB8`, stop `0x472BECEC`, set-voice-recognition-mode `0x472BDDB4`.

### 9.4 TV-side API surface (dnfile on Tizen.TV.Network.Bluetooth.dll + ELF dynsyms)

CAPI (`libcapi-network-bluetooth-tv.so.0.1.04_tv`): `bt_hid_rc_start/extend/stop_sending_voice`,
`bt_hid_send_device_command`, `bt_hid_send_mbr_command/data`, `bt_hid_send_haptic_command/data`,
`bt_hid_get_smart_rc_{address,device_type,fw_version,battery_status,language_version,grip_sensor_state}`,
`bt_hid_set_smart_rc_voice_recognition_mode`, `bt_hid_set_{audio_data,far_field_voice_data
(,_with_noise),ble_voice_info,mbr_return,smart_rc_send_cmd_status,smart_rc_charging_status,
smart_rc_grip_sensor_state_changed}_receive_cb`, `bt_adapter_{set,get}_lpm_mbr_mode`,
`bt_hid_update_smart_rc_fw`.
.NET structs: `BtHidVoiceDataStruct{length,audio_buf}` (8 B),
`BtHidMbrReturnStruct{length,mbr_return}`, `BTHidBleVoiceInfoStruct{speech_power_level,
signal_noise_ratio}`, `BTHidFarFieldVoiceDataStruct{length,voice_buf}`,
`SmartRcSensorDataStruct{sampling_time,sensor_type,touch_mode,x,y,z}`,
`SmartRcThroughputDataStruct{total_bytes,total_msecs,throughput}`; enum values incl.
`BT_HID_DEVICE_16_SMART_CONTROL_TYPE1_BLE` (=49), types 58/65/66 (mic-capable
  successors).

### 9.5 Live-verified TV behavior (docs/research-notes/VOICE_RECORDING_TRACE.md)

- `bt_initialize` → `bt_audio_initialize` → `bt_hid_host_initialize` →
  `bt_hid_set_audio_data_receive_cb` → `bt_hid_rc_start_sending_voice` chain
  returns 0 (success) on a live TV; the paired remote (2016 Smart Control,
  device type 49) delivered **0 bytes**. The remote does have a MEMS mic
  (teardown, 2026-11-02) and a MIC key (`0xA0`); the likely cause is that the
  start was sent without MIC held, which the remote drops (§9.6).
- Mic-capable remotes are expected to deliver 16 kHz mono s16le in the callback.
- Key code 462 = `KEY_VOICECOMMAND` (tools/voice_poc.py default).
- **Transport generations** (from the same trace): older mic-capable remotes
  stream over **classic-BT SCO/eSCO** (kernel `btpcm` module, `/dev/btpcm0`,
  TV-side `avocd` forwards to CAPI clients); 2021+ solar remotes (incl. Y25)
  stream over **HID report 0xFB**. Both are triggered by the same CAPI call.
  ESP32/Linux hosts can only reach the HID-report generation.

### 9.6 Remote-side command dispatcher (deep trace, 2nd pass)

Traced in the remote's own firmware (`remote-fw/bin/SC_BT_Y25_20592200.bin`
and `SC_BT_Y25_EA592200.bin`, same addresses, bank0). The live remote reports
type `EA`, version `592200` in its Dev Info reply, so the `EA` image is exactly
what runs on it. Corrected 2026-11-02: the first pass took `31 01 17` for the
voice start; it is the device info query (see the voice handler below).

- **Group dispatcher** `0x0B5C8`: `r1` = payload buffer, dispatches on
  `payload[0]`:
  - `0x20`/`0x22` → handler `0x0B334` (MBR: reads `[p+1..3]`, calls `0x0D6A2`;
    values `0x00`/`0xFF` of `[p+3]` branch to `0x8D12`/`0x8D38`)
  - `0x31` → **voice handler `0x0B46C`**
  - `0x34` → sub-command handler `0x0B51E` (accepted subs:
    `04,05,06,07,1A,1B,20,21,22,30,31` — calls `0x0E9BC`, `0x48AC`, `0x4964`,
    `0x0D40A`, `0x0D542`, `0x0D580`, `0x0D596`)
- **`0x31` handler** `0x0B46C` dispatches on `payload[1]`:
  - `1` → **device info query**: requires len ≥ 2 and `payload[2] == 0x17`
    (`cmp r0,#0x17` at `0x0B49A`). `0x0B438` sends the Dev Info reply (sub-ID
    `0x33`) on the `0xFB` Input report. Then, only if byte 0 of the state
    struct `0x20B234` is still 0, it sets it to 1 and calls `0x059BC`, which is
    a ROM config save (`0xFF7D5F32(ptr, 0x100, 8)`; `0x059CA` is the matching
    load) — a persisted "host queried device info" flag, not a stream arm.
    Live: every `31 01 17` got `33 17 00 03 00 00 02 00 02 31 a3 00 00 00 ea
    59 22 00 11` and no audio. TV side: sent by
    `__bt_service_fetch_hid_device_info`.
  - `2` / `3` / `4` → post voice event (`0x7EF2(2, &param)`) with param
    `2` / `1` / `0` = **stop / extend / start**. (`4` is not "state byte = 0":
    the 0 is the event parameter.) `6` is not handled.
  - `0x24` → takes `payload[2]` as parameter, calls `0x47B0` (candidate:
    voice-recognition-mode)
- **`0x34` handler** `0x0B51E`: requires `payload[1] == 0x31`, dispatches on
  `payload[2]`; `0x1A` / `0x1B` arm / clear timer 5 via `0x3D1A` = no-latency
  on / off (TV `__bt_set_ble_conn_no_latency` sends `FB 34 <type> 1A|1B 00 00`
  with type `0x31` for this family).
- **Voice event handler** `0x7F40` (event 2): param 0 (start) is accepted only
  if the gate byte `0x20B9DC+1` is 1, otherwise silently ignored. The gate is
  set by `0x7E08` (MIC key, key index 6: sends the key report, arms a 1000 ms
  timer, sets the gate) and `0x7EA4`, and cleared by `0x7E82` on the
  all-keys-released path (`0x73A6`, `0x755C`). So **start must arrive while
  MIC is held** (max 1 s). Accepted start: stop the gate timer, `0x7D6C`
  (allocate stream state via `0x5928(4, 0xAC, 0)`, `0x79A0` sets the I2S pin
  mux: GPIO `0x17`/`0x15` → functions `0x60`/`0x61`, plus pin `0x24`), arm
  stream timer 2, set streaming flag `0x20B9E8 = 1` (`0x7DE2`). Param 1
  (extend) restarts the timer if streaming; param 2 (stop) runs `0x7D14`.
- **Audio capture/encode is in mask ROM**: the flash image has no IMA-ADPCM
  step/index tables and no codec strings; `voice_handle_notify_voice_data`
  is only a name in the trace-name pocket. Live (ESP32, 2026-11-02): with
  `34 31 1A 00 00` + `31 04` sent 160 ms into a 2.7 s MIC hold, every write
  was ATT-acknowledged but no `0xFB` audio arrived. The missing condition is
  not visible in the flash image.
- **Entry path into the dispatcher** (`0x03AB6` BTIF message handler):
  event type 3 with `[r4+4] == 1` → passes `(len, buf)` from the GATT/HID write
  event into the dispatcher. The dispatcher is fed by the HID **write/
  SET_REPORT** event path (BTIF `handle_attr_write_req` family), consistent with
  the Feature-0xFB channel; the vendor `0xA002` characteristic has separate
  write callbacks (`0x34F84` expects ≥16-byte structured payloads — sync/OTA
  family, not voice).
- **TV-side cross-check** (bt-service; the addresses in §9.2 and here are
  file offsets, virtual address = offset + `0x10000`): `0x405E6` (vaddr) /
  `0x4B722` build `FB 31 <cmd>`, appending `01 17` only for cmd 1
  (`movw r3,#0x1701`). `__bt_send_voice_cmd` (`0x407AC`) is called with cmd
  `4` (voice key `0xD8`, start), `6` (key `0x212`, stereo start) and `2`
  (key release / stop paths `0x40A78`, `0x40BCE`); cmd 1 comes from
  `__bt_service_fetch_hid_device_info` (`0x4B84E`). Transport:
  `__bt_smart_rc_set_report` (`0x4B154`) hex-encodes the buffer and sends it as
  a Feature report ("Feature Report [%s], result = %d"). Wake-up path re-sends
  start "must be first".
  TV incoming-report dispatcher `0x3031A` accepts group bytes `0x1F–0x34`
  (tbb table `0x3032C`), and audio frames are routed to the ADPCM decoder
  (`__bt_decode_adpcm_data`).

Cross-generation corroboration (external, see REMOTE_PROTO.md §6.1): Apple's
Siri Remote is host-activated the same way (Siri hold alone → zero audio
reports; Feature-report enable; macOS hides HID service from apps — the
Linux/CouchVox/SiriRemoteForge work), and Android TV remotes stream ADPCM over
HID reports (`hid-atv-remote` kernel driver). The host-gated mic over HOGP is
an industry pattern.

---

### 9.7 TV-side remote-pairing trigger (source of REMOTE_PROTO.md §2.5)

- Keyrouter rule —
  `usr/share/enlightenment/data/config/tizen-tv/module.keyrouter.cfg`:
  `source selector="key" override="true">XF86BTPairing` →
  `target selector="app" request="force-launch"
  args="type:XF86BTPairing">org.tizen.SmartRC`, condition `app not alive
  org.tizen.SmartRC`. Same pattern file also routes `XF86BTPairingGuide`.
- Key identity: X11 keycode **153** (`usr/share/X11/xkb/tizen_key_layout.txt.tv`;
  `<I161>=161` in `keycodes/tizen`); Dali extension enum **`0xA0`** — lookup
  table in `usr/lib/libdali-key-extension.so.0.1.0` at `0x3250`
  (`0x9F→XF86BTColorMecha`, `0xA0→XF86BTPairing`, `0xA1→XF86CinemaMode`).
- `org.tizen.btapp` (usr/apps/org.tizen.btapp/bin/btapp): `OnPairingTriggerPressed`,
  `ConnectRequestByEasyPairing`, `RecoverySmartRc` HSM; IR-received keys land
  as evdev/X events through the micom path (`tztv-micom.ko` holds the IR scan
  table internally — scancode for the pairing key unknown).
- `org.tizen.SmartRC` (usr/apps/org.tizen.SmartRC/bin/SmartRC):
  "XF86BTPairing key pressed..Launch SmartRC Pairing..Set UI Mode == TRUE",
  discovery popup states (`SMART_CONTROL_POPUP_STATE_DISCOVERY_START`,
  `..._DEVICE_NOT_FOUND`, `..._DEVICE_PAIRING_FAILED`),
  "RemoteAddrs found in discovery %s", app_control extra key `type`.
- Launch syntax evidence: `/usr/bin/app_launcher -f <appid> -u <uid>
  [k v …]` — from `usr/bin/appstore.sh` and `usr/bin/art_start.sh`;
  owner uid 5001 (`/run/user/5001` in this repo's TV agent paths).
- Consequence: a replacement central cannot be discovered without the TV
  launching its search (key event or app launch). The ESP32 relay's earlier
  pairing attempts failed purely for this reason (monitor logs show
  "pairing mode timed out", zero TV connections ever).

---

## 10. Input hardware RE: is the PCB "spring" a touch sensor?

**Question raised:** the remote PCB has a spring-like structure — is it
capacitive touch sensing?

**Answer: No.** The Y25 remote firmware contains no capacitive-touch driver at
all (checked across all 10 hardware-type variants). The spring is almost
certainly the **BLE PCB antenna** (a meandered/MIFA trace looks like a flat
spring) — or, if it is a physical coiled wire, a battery/solar-cell contact.

### 10.1 What is *not* in the firmware

- `CAP_Touch` appears **only** as an IRQ name in the SoC's built-in
  peripheral-name table (`0x12d30`, between `MemManage` and `Keyscan`) — a
  platform capability of every Realtek Bee SoC, never referenced by app code.
- **No touch/cap/grip/sensor handler anywhere**: the app trace-name pocket
  (`0x12388`) lists exactly `app_main_task`, `app_send_msg_to_apptask`,
  `loop_queue_init`, `event_mgr_cback_register`,
  `voice_handle_notify_voice_data` — no touch, no sensor, no grip.
- `app_set_threshold_handler` (`0xd460`) sits inside the **factory MP-test
  command parser** next to `[uart_test_enter_*]` and
  `MP extension test K RF, rx_chan %d, threshold %d` — it's the RF test
  threshold setter, not touch sensitivity.
- The RAM config struct at `0x002002E0` (with "channel/threshold"-looking
  fields, init'd at boot via fn `0xc34`) is `bt_stack_config` — its init prints
  `[bt_stack_config_init] test mode = %d` (string at `0xc5c`, passed to a ROM
  print call; test-mode bits come from a config word, `==3` sets a bit in
  `0x00200318`).

### 10.2 Buttons are a mechanical key matrix (Keyscan peripheral)

- `Keyscan` IRQ + `keyscan_timer` (`0x68a8`) + `long_press_key_detect_timer`
  (`0x7208`) → periodically-scanned physical matrix.
- A **per-model key-map table** lives at `0x123DA+` (type-0x20 image):
  `02 03 0f 10 0e 16 06 01 0e 0f 04 0a … 09 [2b] 28 00 03 0c 13 18 08 1e`,
  followed by 6-byte long-press records `03 <key> 07 00 <key2> 00`
  (keys 0x12, 0x10, 0xB9/0x79, 0x68, 0x58, 0x65, 0x62, 0x60, 0x61, 0xEF, 0x0F,
  0x07, 0x0B, 0x3F, 0xD2, 0xD1, 0x4F, 0xF3, 0xF4, 0x37, 0x9E, 0xDF, 0xFD,
  0xB4, 0xBF, 0xDD, 0x9D, 0xCD, 0xFC, 0x6F, 0x9B, 0xFA, 0x70, 0x71, 0x6A).
- **The one code byte that differs per remote type** (`0x123EE`) is inside this
  key-map: `0x2B` for types 20/E9/EA, `0x00` for E1–E6, `0x73` for E7,
  `0x01` for F3/F5 — i.e. **different button sets per hardware model**, not
  different sensors.

### 10.3 Touch *protocol* exists, but as legacy only

The HID Report Map (`0x11ef0` in the type-0x20 image, identical in all 10
variants, both banks) contains a **Mouse/touchpad collection (Report ID 2,
buttons 1–3, relative X/Y −127..127)** — decoded report catalog:

| Report ID | Size | Collection | Role |
|---|---|---|---|
| 1 | 8B | Keyboard | legacy |
| **2** | 5B | **Mouse/Pointer (X,Y + 3 btn)** | **touchpad-era legacy — unused** |
| 0xF6 | 180B | Consumer vendor | Samsung vendor input |
| 0xF7 | 6B | Vendor FFF0 | Samsung key report (✅ live-confirmed) |
| 0xF8 | 6B | Vendor FFF0 | Dev Info / battery |
| 0xF9 | 18B | Vendor FFF0 | sensor/touch_mode report (matches TV-side `SmartRcSensorDataStruct{sampling_time,sensor_type,touch_mode,x,y,z}`) |
| 0xFB | 192B+11b | vendor | voice audio streaming |
| 7 | 20B signed | Vendor FF01 | motion/accel IN |
| 8 | 20B | Vendor FF01 | TV→remote control OUT |

The fw lineage literally says `Smart Control 2016` (string at `0x12372`) — the
2016–2019 Samsung Smart Touch remotes **did** have capacitive touchpads, and the
mouse collection is their protocol remainder. The TV side still parses touch
reports ("Invalid touch input report received") and exposes
`bt_hid_get_smart_rc_grip_sensor_state` / `touch_mode` in the sensor struct —
protocol support for other remote generations, not this hardware. Live sniff
(`REMOTE_PROTO.md` §4.2) confirms Report ID 2 is never emitted by this remote.

### 10.4 Hardware verdict & quick tests

**First, identify your SoC generation** (check the marking on the main QFN):

| SoC on board | Remote generation | Firmware source |
|---|---|---|
| **Atmosic ATM2231U** (+ Winbond 25Q40EWNIG 4Mbit flash) | 2021–2023 solar remotes (e.g., BN59-01391A) — LiPo *soldered* to the board (4 joints), no battery springs | not our dump |
| **Realtek RTL8763B-family** | 2025 (Y25) — the 10-type fw analyzed here | `remote-fw/bin/` |

(Externally-teardown evidence for the Atmosic generation: makingstudio.blog
BN59-01391A teardown.) The no-touch verdict holds for both, but only the
Realtek board is covered by this fw analysis.

- Flat zigzag/serpentine **copper trace** near the PCB edge → **BLE antenna
  (MIFA)**. Test: continuity — one end goes to the SoC RF pin (often through a
  small π-match network: inductor + 2 caps), far end is *open*; covering it
  with a hand drops BLE RSSI noticeably.
- A physical **coiled-wire spring standing off the board** (usually at one end,
  one end soldered, other end free) → **helical BLE antenna** — a standard,
  catalog part for 2.4GHz remotes (e.g., CTRF-ANTENNA-SP-2450: ~13×9mm copper
  coil, λ/4). Test: one end traces to the SoC RF/matching network, the other
  end connects to *nothing*; grabbing it with your hand kills BLE RSSI.
- Physical **coiled wire spring** → battery (−) or solar-cell contact; test:
  continuity to the Li-ion terminal / charger IC.
- A real capacitive touch electrode would connect to a capsense-capable GPIO
  (and the fw would init CAP-Touch channels — it doesn't, in any variant).

Cross-checks: `RemotePolicy`/live traces in `REMOTE_PROTO.md` (Report 0xF7 used
for keys; 0xF2 voice; touch unused).

## 11. Hidden / non-obvious remote functions (RE'd)

Beyond keys, voice, OTA, and MP/debug — found by RE of the remote fw plus the
TV-side daemons that drive it.

### 11.1 Universal remote / IR blaster — full ZipIR stack (biggest find)

The remote is a **programmable universal IR blaster** ("One Remote" feature):

- Remote fw contains the complete **ZipIR** library (strings `0xf8b8–0xfec8`):
  init/decode/transmit, `ZipIrBlastSignal start, function: %d, frameType: %d`,
  `ZipIR get KeyCode, phy_addr: 0x%x, request size: %d` — an **IR codeset DB
  stored in the remote's own flash** at a physical address (partition beyond
  the 535KB OTA image — not in the `.hex` dump).
- TV side: **`qdinfo-daemon`** + **`libauto-codeset-manager.so.1.1.0`** (dlopen'd
  by qdinfo):
  - "Codeset Install Start" / "GenerateAndSendSetupData" → **codesets are
    pushed to the remote over BLE HID** ("Send Exit Remote Control with %d
    codesets" removes them again)
  - **"Info-frame found one codeset"** — auto-detects the attached soundbar/STB
    from **HDMI info-frames** and auto-installs the right codeset
  - `ByPassIRSend` paths, remote fw version gates ("2018 SC a07", "2019 SC"),
    `com.samsung/featureconf/cx_core_ir_one_remote` feature flag
  - `CQDInfoWizardManager` — an on-screen universal-remote setup wizard
- So key presses in "universal" mode are looked up in the on-remote codeset DB
  (`KeyFlag + function_code` → keycode) and blasted via GPIO+timer modulation.

### 11.2 Self-healing watchdog (SW PROTECT)

`[SW PROTECT] rcu_sw_reboot, reset_reason: 0x%x` — the remote runs a software
watchdog with per-subsystem timers: `sw_protect_start/stop_adv_timer`,
`sw_protect_le_send_timer`, `sw_protect_disconn_timer`, and notably
`sw_protect_exit_ir_timer` (**if an IR blast hangs, the remote reboots
itself**). Reset reason is kept in retention RAM and presumably reported to
the TV in the Dev Info report.

### 11.3 Thermal tracking

`THERMAL_TIMER` (IRQ name table) + `thermal_tracking_register_callback_func`
— SoC thermal monitoring on a remote (the RTL8763B DSP path can heat during
voice streaming/charging).

### 11.4 Google Fast Pair infrastructure (latent?)

`mp_test_set_fast_pair_info` + `[uart_test_enter_fast_pair_mode] Switch to
Fast Pair %d test mode!` — the factory MP flow can write Fast Pair info
(model ID / keys) into the remote and test FP mode over UART. No FE2A UUID
table in the adv data of this build — FP looks provisioned-but-dormant
(likely for phone-assisted onboarding in some SKUs).

### 11.5 Voice-firmware & "language pack" channels

- `bt.updatemanager` has a **third update channel**: staging dir
  `/opt/usr/apps/com.samsung.tv.farfieldvoice/shared/res/sc_firmware`
  (farfieldvoice app, `ffv-service` + `libVPP_VD.so`), next to
  `/opt/data/ci_fw_update/smartrc_fw` and the `SCFW` D-Bus method.
- The remote reports a **`language_pack_version: 0x%x`** (TV persists it in
  `file/bluetooth/bt_smart_rc_language_version`) — a versioned, separately
  updatable pack (no language strings exist in the Y25 fw itself — likely for
  voice-model/other generations).

### 11.6 Factory voice test loopback

TV-side test rig: `memory/bluetooth/voice_test` + `BT_VCONF_VOICE_TEST_STATUS`
vconf keys, and bt-service streams **`/tmp/test_audio.wav`** into the voice
path ("Unable to open Voice input file[/tmp/test_audio.wav]") — an end-to-end
mic/codec test without a human talking.

### 11.7 Assorted smaller ones

- `power_trigger_pairing_timer` — power-key-triggered pairing/reconnect path.
- `bat_redetect_timer` + `repeat_meas_bat_timer` — ADC battery re-measurement
  (handles solar/USB floating readings); `usbchg_*` debounce timers for
  USB-C charge state.
- `led_ctrl_timer` — TV-controllable LED behavior (charge/pairing status).
- TV-side `__bt_smart_rc_send_touch_event` and
  `bt_hid_get_smart_rc_grip_sensor_state` — legacy/other-generation remote
  APIs still present.
- `long_press_key_detect_timer` + per-type key-map (§10.2) — hidden long-press
  combos (e.g., the per-type byte `0x2B`/`0x00`/`0x73`/`0x01` at `0x123EE`
  selects *which* buttons exist per hardware model).

### 11.8 Codeset-push channel: security analysis (disassembled)

The universal-remote transfer path (qdinfo-daemon → libauto-codeset-manager →
bt-service → BLE HID → remote) was disassembled. Findings:

**Ecosystem**: it's **UEI QuickSet** (`cmd.conf`: "UEIC Command App Config
file", `dbroot:=/usr/share/qse`, `oemid:=Samsung_ZipIR_2020`,
`cacert:=/usr/share/qs/CAcert_UEI`). TV-side DB: `/usr/share/qse/*.db`
(encoded, e.g. `cce_encoded.db`, `dac*`, `CECINFODB.txt`), engine = libqs
(`qse_generate_command`, `qse_generate_codeset_buf`,
`qse_qsp_get_infoframe_device`). Codesets also downloadable online
(`IsForceOnlineCodeset`, `SetDownloadFinished`; TLS pinned to UEI CA cert).

**Frame builder** — `CQDInfoDeviceProtocol::GenerateAndSendSetupData`
(libauto-codeset-manager.so @ `0x9584`):

```
size check:   total ≤ 0x9C3 bytes, else bail
device gate:  strlen==0x42 || 0x51 (device_type; else cmd=1 path)
frame (state struct @r5):
  +0x04  int  param
  +0x08  u8   cmd (=3 setup / 1 other)
  +0x0a  u8   param2
  +0x0c  u16  strlen(codeset_str)  [big-endian, rev16]
  +0x0e  u8   1 (seg flag)
  +0x0f  u8   segment index (0,1,2…)
  +0x10  u8   chunk size (≤0x64=100 bytes)
  +0x11  char codeset_str[≤0x6d]   (copied, 109B max)
  +0x19  data[≤100]                (raw memcpy from vector)
  +0x180 u32  frame length = chunk+9
  +0x184 u32  1 (pending flag)
```
- Chunked: **100 data bytes per frame**, segment counter at +0xf, loop until
  remaining==0.
- **NO CRC, NO MAC, NO signature computed anywhere in the builder** — it's a
  raw struct fill + memcpy loop.

**Transport** (ELF JUMP_SLOT relocs): `bt_hid_send_mbr_data`,
`bt_hid_send_mbr_command`, `bt_hid_send_device_command` — direct Tizen BT CAPI
calls (qdinfo-daemon in-process; no D-Bus hop). bt-service routes them as HID
OUT/SET_REPORT to the remote; return path = `HidMbrReturnPktReceived` /
`__bt_handle_mbr_return_report`.

**TV-side "validation"**:
- `IsValidCodeset` (@0x5a98) — checks **exactly one thing: first char of the
  codeset string is 'A'..'Z'** (`c-'A' ≤ 0x19`).
- `CheckLocalCodeset` — list lookup (existence, not security).
- Size gate 0x9C3. Nothing else.

**Remote side (Y25 fw)**: the ZipIR pocket contains only decode/transmit
strings (`get KeyCode phy_addr` → codeset DB is **factory-preloaded flash**;
`IrConvertToWaveForm` → remote converts encoded commands to GPIO waveform).
**No install-path or checksum strings anywhere** in the fw; the only
signature check in the whole image is the DFU OTA magic `0x8721BEE2` —
**not** applied to this channel.

**Conclusion / attack surface**:

| Layer | Protection found |
|---|---|
| Codeset string | first char 'A'–'Z' |
| Frame integrity | **none** (no CRC/MAC/signature) |
| Origin auth | none beyond BLE LTK (link encryption from pairing) |
| Pairing model | headless (no PIN) — TV keeps `bt_headless_info_smart_rc1/2` + `.headless.db` (hash-checked), `auto-pair-blocklist` of vendor OUIs |
| Remote-side parse | unknown (no strings; likely ROM/fuzz target) — size fields (BE16 len, ≤0x64 chunk) are attacker-influenced once you're the link peer |

Practical reading: the channel is only as trustworthy as the BLE link key.
A rooted TV can already send arbitrary "blast" frames (IR spoofing of any
codeset — e.g., bricking/controlling other devices' IR inputs is by-design).
The juicier angle: the **frame parser on the remote** (BE16 length + 0x6d
string + 0x64 chunk into fixed buffers) is a classic overflow candidate, and
there's no memory-safety evidence either way — worth a fuzzing harness once
the HID OUT report ID/format is confirmed on-air.
