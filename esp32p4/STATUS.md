# Xcertplay ESP32-P4 Wireless CarPlay — Project Status & Handoff

> Written to allow a fresh session/agent to continue without losing the hard-won
> protocol knowledge. Last updated: 2026-09-30 — bplist writer fixed (the phone
> stall root cause), CSeq/keepAlive/feedback aligned with the references.

## 1. Goal

Reimplement the wireless CarPlay receiver from `xcertplay` (Android/Kotlin, GPLv3)
on an **ESP32-P4-WIFI6-Touch-LCD-4.3** board (480x800 MIPI-DSI, GT911 touch) with
**LVGL**, plus an original **ESP32 (classic BT)** as the Bluetooth front-end and an
onboard **MFi** coprocessor for authentication. Wired CarPlay and audio/HID are
out of scope for now.

Reference implementations (both verified working with a real iPhone):
- `xcertplay/` — Kotlin, the porting source (in this repo).
- `..\reference\LIVI\` — TypeScript/Rust, used to cross-check protocols (sibling directory).

## 2. Hardware & wiring

| Item | Detail |
|---|---|
| Board | ESP32-P4-WIFI6-Touch-LCD-4.3, **chip rev v1.3** (needs `REV_MIN_100`, 200 MHz PSRAM) |
| Classic-BT bridge | ESP32 devkit, UART link: **P4 GPIO4 (TX) -> ESP32 GPIO16 (RX2)**, **P4 GPIO5 (RX) <- ESP32 GPIO17 (TX2)**, 5V + GND common, 115200 |
| MFi module | MFI343-class part at I2C 0x10 on **SDA=GPIO3, SCL=GPIO2** (bit-banged, dedicated bus) |
| P4 serial console | COM27 (USB-TO-UART) |
| ESP32 serial console | COM28 |
| iPhone AP | SSID `xcertplay-CarPlay`, pass `xcertplay123`, ch 6, IP 192.168.4.1 |

## 3. Repo layout

```
ESP32-P4-Xcertplay/            <- main ESP-IDF v6.0.3 project (target esp32p4)
  components/xcertplay_core/       crypto (PSA + libsodium Ed25519), TLV8, SRP-6a, bigint, identity(NVS)
  components/xcertplay_platform/   Wi-Fi softAP, mDNS (_airplay._tcp), MFi bit-bang driver, I2C scan
  components/xcertplay_airplay/    bplist writer+reader, RTSP server, /info, pair-setup/verify,
                                   control cipher, /auth-setup, /stream SETUP, aux (NTP/event/keepalive),
                                   video receiver + H.264 decode + panel render
  components/xcertplay_bootstrap/  UART link to the BT bridge (A5 framing), iAP2 link layer + CSM,
                                   wireless bootstrap state machine (identification, MFi, Wi-Fi creds)
  tools/airplay_pair_test.py       PC-side full-chain test (pairing, control cipher, /auth-setup, video)
  tools/setup_reply_compare.py     captures the device-info SETUP reply bytes and byte-compares them
                                   with a Python port of xcertplay's BplistCodec encode()
  tools/mp4_extract_h264.py        extracts avcC + length-prefixed samples from an MP4 for video tests
  tools/test_video.h264p           generated H.264 test stream (58 samples, from LVGL birds.mp4)
  ..\reference\LIVI\              LIVI reference clone (sibling directory)
  xcertplay/                       xcertplay reference clone
..\xcertplay-bt-bridge\        <- classic-ESP32 firmware (target esp32): GAP/SSP + HID mouse + SPP client
..\xcertplay-mfi-diag\         <- standalone MFi diagnostic project (v1..v9 experiments)
```

Build/flash (activate the EIM profile first):
```powershell
. "C:\Espressif\tools\Microsoft.v6.0.3.PowerShell_profile.ps1"
idf.py -p COM27 -b 460800 flash monitor      # P4 project
idf.py -p COM28 -b 460800 flash monitor      # ESP32 bridge project
```
PC-side full test (PC must be on `xcertplay-CarPlay`):
```powershell
& "C:\Espressif\tools\python\v6.0.3\venv\Scripts\python.exe" tools\airplay_pair_test.py 192.168.4.1 7000
```

## 4. Verified working (on hardware)

- **M0** display + LVGL 9 + GT911 touch (BSP), crypto/TLV8/bigint self-tests pass.
- **M1** softAP via ESP-Hosted/C6 (`esp_hosted 2.12.13` + `esp_wifi_remote 1.6.4`).
- **M1b** mDNS `_airplay._tcp` with real Ed25519 `pk` and UUID `pi`.
- **M2** MFi @0x10: certificate 608 B read + 64-byte challenge signature.
- **M4a/M4b** AirPlay RTSP (`/info` plist), `/pair-setup` (SRP-6a 3072 HW-accelerated),
  `/pair-verify` (X25519 + Ed25519), all verified against a PC client.
- **M5** control-channel ChaCha20-Poly1305 framing + `/auth-setup` (real MFi cert+signature).
- **M6** video channel: 128-byte header framing, ChaCha decrypt, length-prefixed->Annex B,
  H.264 SW decode (tinyh264) and DSI blit — verified with 58 real frames from the PC and an
  on-device HW-encode loopback (480x272 decoded + drawn).
- **Wireless bootstrap (esp32 bridge)**: iPhone pairs (HID trick), bridge connects to the phone's
  iAP2 RFCOMM channel (ch 1) and relays it over UART; P4 completes iAP2 identification, MFi auth,
  Wi-Fi credential delivery (`0x5703`) and `CarPlayStartSession` (`0x4301`). The iPhone then joins
  the hotspot and starts the full AirPlay session: `/info` -> pair-setup -> pair-verify ->
  encrypted `/auth-setup` -> `SETUP` (device info) -> our reply accepted.

## 5. CRITICAL protocol knowledge (learned the hard way)

### MFi (MFI343 class)
- **Needs >= 5 ms between the register-select write and the read.** Hardware I2C (us gaps) NACKs
  every read. Driver is bit-banged (`xc_mfi.c`), ~50 kHz, `SEL_SETTLE_US = 6000`.
- Registers 0x00-0x2F readable with separate write/read; 0x30/0x31 (certificate) only with
  **repeated START**; the chip locks the certificate window after the first sign until power cycle.
- During signing the chip NACKs all reads for ~1.6 s: poll `0x10` until `0x10`, don't give up.
- Protocol major 3 -> SHA-256 digest for `/auth-setup`.

### iAP2 link layer (over the bridge)
- Marker `FF 55 02 00 EE 10` (send 1/s until the phone answers).
- Packet: `FF 5A | len u16 BE | ctrl | seq | ack | session | hdr_csum` + payload + payload_csum.
  **No payload checksum when the payload is empty** (len == 9).
- ctrl 0x80 = synchronize (PSN **99**, does NOT consume a PSN), 0x40 = data/ack, 0x10 = reset.
- **First data packet must be PSN 100** (99 + 1). Standalone ACKs use **session 0**, current PSN,
  no increment. Data goes on session 10.
- Sync payload: `01 04 FFFF 0FA0 01F4 04 03` + sessions (10,0,2)(11,2,1)(12,1,2).

### iAP2 CSM / messages
- CSM frame `40 40 | len u16 BE | msgid u16 BE | TLV body`.
- TLV: `len u16 BE | id u16 BE | value`; `len` includes the 4-byte header; group = nested TLVs.
- **`u16List` = raw big-endian u16 array; `strings` = concatenated NUL-terminated strings**
  (not nested groups).
- Identification 0x1D01: params 0-10, 12, 13, 17(BT transport, **MAC little-endian**),
  20/21/22/30 optional; the phone answers `0x1D03` listing rejected ids -> **drop the whole TLV**
  and re-send (droppable: 20, 21, 22, 30).
- MFi: `0xAA00` -> `0xAA01` (cert), `0xAA02` -> `0xAA03` (signature), `0xAA05` = success.
- Wi-Fi: `0x5702` -> `0x5703`; CarPlay: `0x4300` -> `0x4301` (SSID/pass/channel/IP/port/deviceId/pk/srcvers).

### AirPlay
- `/info` binary plist; `features = 0x615653aee2`, `srcvers 950.7.1`, port 7000.
- **Every plist we emit was byte-broken until 2026-09-30** (see section 6): the bplist writer
  over-sized any string/array/dict with count >= 15 by one byte, so /info and the SETUP reply
  were unparseable by CoreFoundation/plistlib. Fixed in `xc_bplist.c:marker_size()`.
- RTSP responses must echo the request's `CSeq` (all working references do). Our responses now
  do; missing CSeq is a silent-stall risk.
- `keepAlivePort` is only advertised when the phone's SETUP carries `keepAliveLowPower == true`
  (xcertplay/LIVI behaviour). `/info` advertises `keepAliveLowPower=false`, so the reply normally
  carries just `timingPort` + `eventPort` + `enabledFeatures`.
- Unsupported streams are omitted from the stream SETUP reply (an empty dict entry confuses iOS).
- Control cipher: `[len u16 LE][ct][tag16]`, AAD = the 2-byte header, nonce = 4 zero bytes +
  8-byte LE counter, read/write keys from `HKDF(shared, "Control-Salt", "Control-Read/Write-Encryption-Key")`.
  The phone encrypts with the key we call *read* — if decryption fails, swap the key pair.
- The phone's `/command` and `/feedback` POSTs arrive on the control connection; unknown requests
  (OPTIONS included) are acknowledged with a bare 200, like xcertplay's unhandled-request path.
- The **phone opens the event channel** (TCP to our `eventPort`) right after the device-info
  SETUP reply and **before** the stream SETUP. The event channel is `Events-Salt`-derived
  ChaCha20-Poly1305 and is only needed when the accessory pushes HID; accepting and draining the
  socket is enough until touch support lands.
- The accessory does **not** need to open `/command` or `/feedback` channels and sends no command
  after the device-info SETUP reply; the next move is the phone's. (Verified against xcertplay
  `AirPlaySession.handleSetup` and LIVI `cpStack.ts` `_handleSetup`.)
- Later the phone sends `SETUP` **with `streams`** -> reply per stream `{type, dataPort}`
  (`/stream` handler already implemented; stream type 110 = main screen).
- Video: 128-byte header (first 4 bytes = body length LE, byte 4 = opcode: 1 = avcC config,
  0 = ChaCha-sealed H.264 frame, AAD = the header, nonce64 counter per connection) then
  length-prefixed NALUs -> Annex B.

### Video decode / display
- tinyh264 (SW decode) is **Baseline only**; the P4 HW encoder emits Baseline and accepts
  `ESP_H264_RAW_FMT_O_UYY_E_VYY` (not YUYV/I420), buffers must be 128-byte aligned.
- Internal RAM is tight: `CONFIG_ESP_H264_DECODER_IRAM=n`, `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
  and `EXT_RAM_BSS_ATTR` on all protocol buffers (ESP-Hosted SDIO mempool fails otherwise).

## 6. Current blocker / next steps (updated 2026-09-30)

**Root cause found and fixed.** The phone's silence was not a missing RTSP channel: neither
xcertplay nor LIVI sends anything after the device-info SETUP reply, and the phone opens the
event channel itself before the stream SETUP. The real problem was that **every plist we emitted
was malformed at byte level**.

`xc_bplist.c`'s `obj_size()` over-estimated string/array/dict objects whose count is >= 15 by one
byte (`marker_size()` returned `1 + int_obj_size()` while `obj_size()` also added its own marker
byte). The SETUP reply contains the 15-char key `enabledFeatures` and the /info root dict has 17
entries, so the offset table (and the table offset in the trailer for longer payloads) was shifted
by one: `plistlib` (and CoreFoundation) could not parse our /info or SETUP reply at all. The phone
apparently tolerated the broken /info (in recent logs it does not even GET /info, likely relying
on the mDNS TXT record) but stalled once the SETUP reply it must parse came back malformed. Fixed
in `xc_bplist.c` (`marker_size` now returns the integer-object size only); verified against a live
capture with `tools/setup_reply_compare.py` (byte-for-byte equal to a Python port of the C writer,
parsed by plistlib, semantically equal to xcertplay's `BplistCodec.encode` output).

Also fixed in the same pass (deviations from both working references):
- RTSP responses now echo `CSeq` (was missing on every response).
- `keepAlivePort` is only sent when the phone's SETUP requests `keepAliveLowPower` (as in
  xcertplay/LIVI), not unconditionally.
- Unknown requests (`OPTIONS`, `POST /command`, `POST /feedback`) are acknowledged with a bare
  200 instead of 404.
- The NTP listener no longer replies to PT_RESPONSE (the phone answering our clock sync).
- Unsupported stream SETUP entries are skipped instead of replied to with empty dicts.

Verification done (2026-09-30, firmware from this tree):
- `GET /info` -> 2029 B, `plistlib` parses: 17 keys, `features=0x615653aee2`, display
  `features=0xa`, 4 HID devices.
- SETUP device-info reply captured over the encrypted channel: parses to
  `{timingPort, eventPort, enabledFeatures=[iAPChannel, viewAreas]}`, carries `CSeq: 7`, and the
  Python model of the fixed writer reproduces the device bytes exactly.
- Real-iPhone end-to-end retry pending: the board is flashed and waiting; watch for
  "event channel connected" and `/stream SETUP: N stream(s)` (N > 0) after the phone reconnects.

Next steps:
1. Reconnect the iPhone (CarPlay): expect `event channel connected` right after the SETUP reply,
   then the stream SETUP. The `/stream` handler answers screen type 110 with `dataPort` and the
   verified video pipeline takes over.
2. Once the phone's stream SETUP arrives, note that MainAudio (100)/AltAudio(101)/Buffered(102)
   streams will be skipped (audio out of scope) — if iOS insists on an audio stream before
   video, building a minimal audio accept path may be needed.
3. Keep `tools/setup_reply_compare.py` for regression: it prints the exact reply bytes and diffs
   them against the reference encoder.
4. Event channel currently only accepts+drains; when touch input lands, derive the `Events-*` keys
   from the pair-verify shared secret and decrypt/reply per xcertplay `acceptEvent`.

Other pending items:
- ESP-Hosted reports `Host [2.12.0] > Co-proc [0.0.0]` -> consider updating the C6 slave firmware
  (RPC timeout risk under video load).
- The bridge firmware needs the phone paired first (HID makes it visible to iOS). If the iPhone
  stops showing it, `Settings -> Bluetooth -> forget`, then re-pair.
- The P4 softAP occasionally stops beaconing after many phone reconnect cycles (PC Wi-Fi can no
  longer see `xcertplay-CarPlay`); a board reset restores it. Keep in mind when debugging.
