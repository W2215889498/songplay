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
- **The real iPhone sends H.264 High profile (idc=100), level 3.1, CABAC, 480x800** (parsed from
  the captured avcC/SPS). ESP32-P4 has **no H.264 hardware decoder** (only HW encoder;
  `soc_caps.h` has `SOC_H264_ENCODER_SUPPORTED` only, no decoder) and the bundled esp_h264 SW
  decoder (tinyh264) rejects profile 100 and cannot do CABAC. Decoding the phone's stream on the
  P4 requires a different SW decoder (openh264 port) or a different SoC - see section 6.
- The iPhone's video config frame is a **raw avcC record** (first payload byte 0x01), NOT
  `\0\0\0\0avcC` like `tools/airplay_pair_test.py` sends; both shapes are accepted now.
- Internal RAM is tight: `CONFIG_ESP_H264_DECODER_IRAM=n`, `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
  and `EXT_RAM_BSS_ATTR` on all protocol buffers (ESP-Hosted SDIO mempool fails otherwise).

## 6. Current blocker / next steps (updated 2026-09-30, evening)

**The AirPlay bring-up is SOLVED.** The phone now completes the whole wireless CarPlay
handshake against this firmware:

```
SETUP reply -> event channel connected -> GET /info -> RECORD -> POST /command x N
-> SETUP (streams, type 110) -> video stream connected, encrypted H.264 frames flow
```

Root cause of the long-standing stall: **the binary-plist byte format**. Two bugs compounded:
1. `obj_size()` over-estimated string/array/dict objects with count >= 15 by one byte
   (`marker_size()` double-counted the marker byte); and
2. our writer emitted a nonstandard layout: sorted dict keys, a 2-byte offset table even when
   1 byte fits, and a **33-byte trailer** (an extra leading zero) - parsers read the last 32
   bytes so it accidentally worked for `plistlib`, but iOS's parser rejected the message.
   The phone matches the response `CSeq` and then silently closes the control connection.

Fixed by rewriting `xc_bplist_serialize()` to match `xcertplay/BplistCodec` and LIVI exactly:
insertion-order keys, minimal offset-table entry size (1/2/4), **exactly 32-byte trailer**.
Verified on air: `event channel connected` 8-10 ms after the reply, phone's `GET /info` served
(2126-byte encrypted frame, iOS accepted it), `RECORD`, `/command` acks, stream SETUP answered
with `dataPort`, and the encrypted video frames arrive and decrypt cleanly.

Also fixed in this push:
- RTSP responses echo `CSeq`; `keepAlivePort` only when requested; unknown requests get 200;
  NTP listener ignores PT_RESPONSE; unsupported stream SETUP entries skipped.
- Each encrypted response is now ONE ChaCha frame (head+body together, like xcertplay/LIVI).
- iAP2 location: `0xFFFA` (StartLocationInformation) now starts a 1 Hz `0xFFFB` NMEA feed
  (`$GPGGA`+`$GPRMC` skeleton in `xc_iap2.c`); stopping on `0xFFFC`. Without it iOS aborts the
  bring-up after a few seconds (LIVI comment) and re-asks CarPlay availability.
- The selftest decoder is closed after the selftest, and video frames are only fed after a
  decoder was created from the stream's own avcC - before this the selftest's 480x272 decoder
  was fed 480x800 real frames and the P4 crashed (load access fault in the decoder).
- The real iPhone video config is a `raw avcC` payload (first byte 0x01) - now parsed.

**New blocker: the P4 cannot decode the iPhone's H.264 stream.**
- The phone sends **High profile (profile_idc=100), level 3.1, CABAC, 480x800** (stream is
  portrait 480x800; the /info display description may need a width/height check).
- ESP32-P4: H.264 **encoder only** in hardware (`SOC_H264_ENCODER_SUPPORTED`, no decoder cap).
- esp_h264 v1.4.1 SW decoder = tinyh264 = **constrained baseline only**, rejects profile 100;
  the bundled `libopenh264.a` contains only encoder symbols (`WelsCreateSVCEncoder`, no
  decoder). tinyh264 also cannot do CABAC, so patching the SPS/PPS is not viable.

Options for the video path (project-level decision):
1. Port the **openh264 decoder** (welsdec, C++), which supports High/CABAC in portable C.
   Expected ~10-25 fps at 480x800 on the 400 MHz P4 cores - needs integration work and a
   performance spike; smaller/landscape display requests reduce the load.
2. Keep the P4 as the wireless/display board and add a companion SoC with a HW video decoder.
3. Re-scope: use the P4 only for protocol bring-up experiments (current state is a complete,
   reproducible wireless CarPlay session up to video decode).

Next steps:
1. Decide the decode path (openh264 port vs companion SoC vs re-scope).
2. Fix the /info display geometry if the 480x800 portrait stream is not intended (check
   widthPixels/heightPixels vs the 800x480 panel; rotate at render time if needed).
3. `/feedback` currently gets a bare 200; LIVI answers with a plist carrying the stream clock
   (`syncedNtp()`), which the phone uses to place media time - implement before audio/touch.
4. Event channel is accepted+drained; HID/touch needs the `Events-*` keys from the pair-verify
   shared secret (xcertplay `acceptEvent`) plus the events cipher counters.
5. Keep `tools/setup_reply_compare.py` for regression of the plist format.

Other pending items:
- ESP-Hosted reports `Host [2.12.0] > Co-proc [0.0.0]` -> consider updating the C6 slave firmware
  (RPC timeout risk under video load).
- The bridge firmware needs the phone paired first (HID makes it visible to iOS). If the iPhone
  stops showing it, `Settings -> Bluetooth -> forget`, then re-pair.
- The P4 softAP occasionally stops beaconing after many phone reconnect cycles (PC Wi-Fi can no
  longer see `xcertplay-CarPlay`); a board reset restores it. Keep in mind when debugging.

## 7. Ground truth from LIVI.log (2026-09-30) — what the phone does next

A real, successful wireless session (LIVI on a Pi). Log line numbers:

- `L113` accessory starts NTP sync towards the phone's timing port (we already do this)
- `L114` SETUP session reply (timingPort, eventPort, keepAlivePort)
- `L116` **"event channel connected" — 21 ms after our reply the PHONE CONNECTS OUR eventPort**
- `L117` `< GET /info (body 177B)` — phone asks /info; body = `{"altScreenURLs":[ ... ]}`
- `L122` `< RECORD rtsp://.../<id> (body 0B)` — session start, answer `200 OK` empty
- `L126` `< SETUP rtsp://.../<id> (body 252B)` — only now the stream SETUP (screen 110/111)
- `L127` `SETUP screen NATIVE (cluster, dataPort=45403, id=...)` — dataPort comes from our reply
- `L128+` `< POST /command` (many, incl. `disableBluetooth`) — acknowledge with an empty 200 plist

**Diagnosis**: our firmware never sees the event-channel connection nor `/info`, so our device-info
`SETUP` reply is being **silently rejected** by the phone. Everything after it never happens.
Fix the reply first.

**Next steps**
1. Copy LIVI's SETUP-session reply construction exactly (`reference/LIVI/src/main/services/projection/driver/cp/stack/cpStack.ts`):
   exact key set and order (`timingPort`, `eventPort`, `keepAlivePort`; check whether
   `enabledFeatures` is included and with which values).
2. Validate our encoding with `esp32p4/tools/setup_reply_compare.py` (+ `setup_reply_captured.bin`).
3. Add a `RECORD` handler (reply `200 OK`, empty body) — currently 404.
4. Keep logging aux-port connects (`event_task` already logs) to confirm the phone reaches them.

Reference trace committed at `esp32p4/LIVI.log` (449 lines, one full session).

## 8. Evening session 2026-09-30 — cluster support, openh264, current blockers

### 8.1 Done tonight (all flashed and exercised on hardware)

1. **AirPlay bring-up SOLVED** (details in section 6): the phone now completes
   `SETUP reply -> event channel -> GET /info -> RECORD -> /command x N -> SETUP(streams)
   -> encrypted video frames`. Root cause was the nonstandard bplist byte layout
   (sorted keys, 2-byte offset table, 33-byte trailer). `xc_bplist.c` now emits exactly
   what xcertplay `BplistCodec` / LIVI `bplist.ts` emit: insertion-order keys, minimal
   offset-table width, exactly 32-byte trailer.
2. **openh264 software decoder ported** — new component `components/openh264_dec/`:
   - vendored openh264 v2.2.0 `welsdec` (api/common/decoder core+plus), C fallback paths only;
   - PSRAM allocator patch in `memory_align.cpp` (>=4 KB via `heap_caps_malloc(SPIRAM)`);
   - `WelsThreadLib.cpp` / `wels_decoder_thread.cpp` ported for `ESP_PLATFORM` (no sysctl,
     no `pthread_attr_setscope/setschedpolicy`);
   - definition/declaration `int` vs `int32_t` fixes in `decoder_core.cpp`;
   - `wels_dec.cpp` wrapper with a small C API (`wels_dec_create/decode/...`).
   - Result: the iPhone's **H.264 High profile (idc=100) + CABAC 480x800** stream decodes.
     Timing per frame (measured): decode 27-53 ms, render (I420->RGB565 + LCD draw)
     98-160 ms -> ~5-7 fps, render-bound.
3. **Instrument cluster (alt screen, stream type 111) support**, following LIVI's
   `docs/carplay-tutorial/10-L6b-仪表盘Cluster地图.md`:
   - `/info` now advertises TWO displays: main (type 110, MAIN_UUID) + cluster
     (type 111, ALT_UUID `b7e6c5a0-2222-4000-8000-000000000002`), same 480x800 geometry;
   - session `SETUP` reply's `enabledFeatures` now `[iAPChannel, viewAreas, altScreen]`;
   - `xc_video.c` rewritten as two channels: **ch111 decodes+renders to the panel,
     ch110 is accepted and drained** (the phone streams both; the main stream must exist);
   - event-channel **command sender** (`POST /command`, Events-Write key, own counters)
     in `xc_airplay.c`; `xc_airplay_cluster_map_enable()` sends
     `showUI {uuid: ALT_UUID, url: 'maps:/car/instrumentcluster/map'}` +
     `forceKeyFrame {uuid: ALT_UUID}` once the main stream is connected and the cluster
     codec config arrived (LIVI gates on `mainStreamReady` the same way).
   - Without these two commands the cluster stays **black** (map rendering is not automatic).
     NOTE: the earlier "ch111 decoded 480x800 frames" happened **before** the activation
     commands existed, which is why the panel showed nothing.
4. **MFi iAP2 hardening**: certificate send uses the boot cache; the certificate is
   **re-read (prime) immediately before `xc_mfi_sign_challenge()`** in the iAP2 challenge
   handler, mirroring the AirPlay `/auth-setup` path (this coprocessor authenticates only
   right after a fresh certificate-window read).
5. Diagnostics added (temporary, still in tree): `link tx` / `link tx ACK raw=...` logging
   in `xc_bt_link.c`, NTP/ctrl-frame logs in `xc_airplay.c`, `xc_bridge` event relay in
   `xc_bt_link.c`.

### 8.2 Verified live (old iPhone 8, iOS 15.7)

- Full CarPlay session to video: `/stream SETUP: 1 stream(s)` -> `ch111 video config:
  h264 39 bytes (avcC 39)` -> `openh264 decoder ready` -> `ch111 decoded resolution 480x800`
  -> frames with per-frame timings (see 8.1.2).
- iAP2 chain: identification (with the 0x0016 parameter retry) -> MFi cert+signature ->
  wifi config -> transport identifiers -> `CarPlayStartSession` -> phone joins AP ->
  AirPlay session -> cluster stream 111 + main stream 110 connected.
- The bridge -> P4 UART relay works both ways; P4 sends the iAP2 marker every second.

### 8.3 Unfinished / current blockers (in priority order)

1. **New iPhone stalls in iAP2 link negotiation (the current blocker).**
   - The replacement phone answers the marker and sends its synchronize packet
     (`ff 5a ... c0 75 63 00 ...`, PSN 0x75, ack 0x63 = our PSN 99) but **repeats the same
     sync ~3x every 1 s and then goes silent** - it never proceeds to identification.
   - Our side sends `phone sync` -> `iAP2 link writable` and a plain ACK
     (`link tx ACK psn=99 ack=117 raw=...`; format identical to xcertplay `Iap2LinkEngine`
     and LIVI `iap2-link` Rust: ctl 0x40, PSN = sentSequence, ACK = phone's PSN, session 0,
     len 9). The phone apparently does not accept/receive it.
   - Suspects: (a) the bridge is not forwarding our ACK reliably during its 2 s
     "channel stability" window, (b) the new iOS validates something further
     (retransmission during negotiation), (c) ACK timing. Next diagnostic: capture the
     `link tx ACK raw=...` bytes and compare against the LIVI working trace of the same
     phone; consider re-sending the ACK (e.g. twice, +200 ms) during negotiation.
   - Note: the old iPhone DID accept the exact same code path earlier today, so this is
     phone-iOS-version specific.
2. **Cluster map activation not yet verified end-to-end** (needs a session that reaches
   video again; the showUI/forceKeyFrame path was added after the last successful video).
   Verify with Apple Maps navigation active (LIVI doc: only the navigation app renders the
   cluster map; Apple Maps is known-good).
3. **Old iPhone stops engaging CarPlay after repeated MFi rejections** - BT/HID stay
   connected but the phone no longer answers the marker. Recovery that worked: reboot /
   forget+re-pair the phone. With LIVI the user sees "配件不支持" and it recovers alone.
4. **Bridge channel selection**: after an RFCOMM dirty close, `esp_spp_connect` reports
   "already opened (MCB state 5)" for channel 1, the scanner falls through to channel 2 and
   the phone drops it after ~3 s, cycling forever. Improvement: prefer the channel that
   actually answers the iAP2 marker and close wrong ones immediately; reset the bridge
   after dirty sessions. This caused a string of "phone does not answer" red herrings.
5. **Performance**: render (I420->RGB565 + LCD draw) is the bottleneck (~100-160 ms/frame).
   Next step: use the P4 2D-PPA (`esp_driver_ppa` exists in IDF v6.0.3) for the colour
   conversion, or at least a faster scalar loop. Decode is acceptable (27-53 ms).
6. **Protocol side still open**: `/feedback` should answer with a plist carrying the stream
   clock (LIVI `_handle`: `{type, sampleRate}` per active audio stream / `syncedNtp()`);
   event-channel HID/touch (Events-Read/Write keys; `xc_airplay.c` already derives the
   write key); audio streams are skipped entirely (type 100/101/102 -> no dataPort).

### 8.4 Repository / file record

Key files touched in this push:
- `components/xcertplay_airplay/xc_bplist.c` - canonical bplist writer (the unblocker);
- `components/xcertplay_airplay/xc_airplay.c` - CSeq echo, conditional keepAlivePort,
  single-frame encrypted responses, 200-ack for /command & /feedback, NTP filter,
  2-display /info + `altScreen`, event-channel command sender + cluster activation;
- `components/xcertplay_airplay/xc_video.c` - dual-channel rewrite (main drain / cluster
  decode+display), openh264 integration, avcC raw-record parsing, zero-length keep-alive fix;
- `components/openh264_dec/` - NEW vendored decoder component (see 8.1.2);
- `components/xcertplay_bootstrap/xc_iap2.c` - location 0xFFFA/0xFFFB, MFi cert prime;
- `components/xcertplay_bootstrap/xc_bt_link.c` - TX mutex, link tx diagnostics;
- `tools/setup_reply_compare.py` - captures the device SETUP reply and byte-compares it
  with the reference encoder (regression tool for the format bug);
- `STATUS.md`, `LIVI.log` (the working LIVI session trace used as ground truth).

Not in git (see `.gitignore`): `build/`, `managed_components/` (385 MB, re-fetched by
`idf.py reconfigure`), `xcertplay/` and `reference/` (embedded reference clones with their
own `.git`), `*.log` except `LIVI.log`.
