# Xcertplay ESP32-P4 Wireless CarPlay — Project Status & Handoff

> 供新会话/新 agent 无缝接手。最后更新 2026-10-08。**先读第 9 节**（最新状态）。
>
> 现状速览：AirPlay 全流程可用（配对→MFi→/info→RECORD→双流→openh264 解码上屏）；
> 仪表盘 240×400；渲染已换 **P4 PPA 硬件 YUV420→RGB565**（9.9，render ~11-16ms，
> P4 容量 ~30fps）；无线 CarPlay 标准流程齐（iAP2 隧道迁移+订阅，9.8）；**启动提速
> 已重做**（9.9，上电→首帧 ≈14s）；通话卡片已实现（9.9 前）；导航音频仍由手机本地
> 播放（归档见 9.8）；ARP 修复见于重连日志（9.1）。调试/烧录/推送方法与操作备忘见 9.5。

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

> Update 2026-10-07: both boards reflashed; **cluster display verified working on the
> panel** after fixing the activation gate (see 8.3 item 2). Diff in this push:
> `xc_video.c` only (conn_fd sentinel + activation retry).

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
   - Update 2026-10-07: after reflashing both boards the phone engaged again and ran the
     full session (identification -> MFi -> CarPlay -> cluster video). The earlier stall
     was therefore at least partly the bridge's dirty RFCOMM state (item 4); keep this
     item as "watch" rather than an open bug.
2. **Cluster map activation: FIXED 2026-10-07 and verified on the panel.**
   - Symptom: CarPlay connected, cluster stream (480x800) decoded and rendered every
     frame, but the LCD stayed black.
   - Root cause: `s_ch[i].conn_fd` is a zero-initialized static, and the activation gate
     `if (s_ch[0].conn_fd < 0) return;` therefore passed **before the main stream was
     connected**. `showUI`/`forceKeyFrame` were sent ~350 ms before
     `ch110 video stream connected`; iOS ignores an early showUI and the cluster surface
     stays empty/black (LIVI only activates after `mainStreamReady`).
   - Fix (`xc_video.c`): initialize both `conn_fd` to -1 on first use, and retry
     `maybe_activate_cluster()` from the cluster frame path on every frame until it
     succeeds (covers the event-channel-not-ready window too).
   - Verified live 2026-10-07: `ch110 video stream connected` -> `cluster: showUI ...
     sent` -> `cluster: forceKeyFrame sent` -> cluster frames decode/render continuously
     (~8 fps) and the LCD shows picture. For MAP content the phone needs an active
     navigation (Apple Maps), per LIVI's cluster doc; the cluster UI itself renders
     otherwise.
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

## 9. 2026-10-07/08 — ARP root cause, cluster resolution, speed work

> **新会话从这里读起。** 9.1 是这两天最重要的根因发现与修复；9.6 是待办清单。

### 9.1 重大根因：iOS 私有 Wi-Fi MAC 轮换 + P4 陈旧 ARP（"重启 P4 才能连"之谜）

现象：CarPlay 弹窗出现，点"连接"后 iPhone 毫无反应 / 状态栏无 CarPlay 图标、屏幕无画面；
`control connection from 192.168.4.2` 在日志里从不出现；**重启一次 ESP32-P4 后立即恢复正常**。

证据（日志时间线 + MAC/IP 对照）：
- iPhone 使用两个不同的 Wi-Fi MAC 连我们的热点：`14:d0:0d:1d:38:11`（稳定地址，早上正常会话
  用的是它）与 `22:04:68:b8:03:8e`（iOS 随机私有地址）；
- **两个 MAC 都从 DHCP 拿到同一个 IP `192.168.4.2`**（手机复用缓存租约）；
- 手机换 MAC 后，P4 lwip 的 ARP 条目 `192.168.4.2 -> 旧 MAC` 仍然存在：手机（新 MAC）发来的
  TCP SYN 能到达 P4，但 P4 的 SYN-ACK 按旧 MAC 发包 -> 丢失 -> iOS 永远完不成连接；P4 持续
  重传还不断"续命"该 ARP 条目，永不超时自愈；
- P4 一重启 ARP 表清空 -> 立刻可连；PC 全链路测试永远正常，因为 PC 的 MAC 不变。

修复（已编译烧录，代码在树内）：
- `components/xcertplay_platform/xc_net.c`：在 `WIFI_EVENT_AP_STACONNECTED/STADISCONNECTED`
  时调用 `purge_arp_cache()` —— 通过 `esp_netif_tcpip_exec()` 在 lwip 线程内
  `NETIF_FOREACH(nif) etharp_cleanup_netif(nif);`，日志 `ARP cache purged after station change`；
- `xcertplay_platform/CMakeLists.txt` 增加 `lwip` 依赖；
- 注意 IDF 6 没有 `esp_netif_get_netif_impl`（旧代码会 implicit declaration 编译失败），
  所以直接遍历 `netif_list`（`CONFIG_LWIP_SINGLE_NETIF` 未启用，`lwip/netif.h` 可用）。

验证方法（待做）：重连时日志应出现 `ARP cache purged after station change`；彻底验证：
在手机 Wi-Fi 设置里对该网络"忽略并重连"（换随机 MAC）→ 点击 CarPlay 连接——**不重启板子**
也应成功。

### 9.2 仪表盘分辨率实验（结论：分辨率无罪，是 9.1 的 ARP 问题）

- 试过 320x240、240x400：当时都"连不上"。现已确认与分辨率无关：
  - iOS 的失败发生在**发起 AirPlay 连接之前**（日志中连 7000 端口的 TCP 连接都没有），
    它根本还没请求 `/info`（分辨率数据在 `/info` 里，连接建立之后才读）；
  - 用户在 LIVI 上实测：**仪表盘 320x240 完全可用**（LIVI 的 cluster 分辨率是可配置的，
    默认 1280x720、辅助屏默认 800x480，均在 CarPlay 规范尺寸内）；
  - 每次"分辨率改动后失败"其实都伴随板子重烧/复位与手机状态波动，是 ARP 问题的表象。
- 当前固件（已烧录）：仪表盘（alt screen / type 111）＝ **800x480**（LIVI 辅助屏默认值、
  CarPlay 标准尺寸），widthPhysical/heightPhysical=154/86mm；**每屏独立 viewAreas/safeArea**
  （对齐 LIVI `areaDict`：viewArea/safeArea 用该屏自己的宽高计算，不再复用主屏节点）。
- 渲染器新增**居中裁剪**（`xc_video.c render_frame`）：流尺寸大于面板时取中心区域显示
  （800x480 在 480x800 开发屏上显示为中央 480x480；换横屏小屏后即标准全屏）。
- 改回 320x240（用户目标：提帧率）的方法：修改 `xc_airplay_build_info()` 里 cluster 分支的
  `widthPixels/heightPixels`（=320/240）、`widthPhysical/heightPhysical`，以及 `sk2/vk2`
  两组 viewArea/safeArea 里的宽高。代码结构已就绪（dk2/vk2/sk2 独立）。

### 9.3 启动提速（已试过、因误判已回退、待重做）

被回退的优化（回退时以为是它们导致连不上，实际是 9.1 的 ARP 问题）：
1. **P4 把 `xc_iap2_start()` 提前**到 `xc_airplay_start()` 之后立即执行（原顺序：显示→AP→
   identity/mdns/airplay→MFi 自检→crypto 自检→视频自检→iAP2）。实测效果：marker 从 ~11.5s
   提前到 ~5s；完整 iAP2 链（ident→MFi→wifi config→CarPlayStartSession）在 **8 秒内**完成
   （原 19s+）。这是最大的一笔收益（约 -7s）。
2. **自检改为延迟任务**：20s 后执行，会话已建立（`xc_airplay_session_started()`）则跳过；
   任务栈必须 ≥12KB（openh264 解码深调用栈，6KB 会 Stack protection fault）。
3. **桥（`..\xcertplay-bt-bridge\main\main.c`）**：首次连接延迟 3000ms→200ms；去掉无用的
   SDP 发现 + 1500ms 等待（标准 SPP 查询永远查不到 iPhone 的 128-bit iAP2 服务）；失败
   通道后加 200ms 让栈释放 MCB。合计约 -4.5s。
重做建议：**逐项重上，每次只改一项**，用日志时间戳记录 marker→首帧时间线。当前基线时序
（cold boot）：P4 启动 4.5s AP 就绪 → 桥连手机 → 手机 marker ~12s（因为 iAP2 排在自检后）
→ AirPlay 会话 ~13-19s → 首帧。

### 9.4 本轮新增诊断能力（都在代码里，保留）

- `xc_airplay.c`：`control connection from X:port` / `control connection closed`；SETUP reply
  详情；ctrl 帧计数日志；cluster showUI/forceKeyFrame 日志。
- `xc_bt_link.c`：**每个链路层包**的 `link rx ctl/psn/ack/sess/len`（非 CSM 包以前是静默的）；
  `link tx` / `link tx ACK raw=..`（含 hex）；坏 header/payload 校验和警告。注意：链路层纯
  ACK（ctl=0x40, len=0）不需要回复 ACK；手机每秒发的是它在 ACK 我们的位置包，属正常。
- `xc_video.c`：前 20 帧 decode/render 耗时；`cluster map activated` 等。
- 桥：每条 SPP 连接记录前 40 帧 rx（hex，计数每连接重置）。
- 工具：`tools/setup_reply_compare.py`（SETUP 回复字节比对）；`tools/airplay_pair_test.py`
  （PC 全链路：配对→MFi→/auth-setup→视频，当前固件全绿：/info 2116B、58 帧视频）。
- 临时技巧：iPhone Safari 打开 `http://192.168.4.1:7000`，在 P4 日志看 `control connection
  from 192.168.4.2` —— 判定"手机→P4 TCP 通路"是否可用（当时未做，可复用）。

### 9.5 环境/操作备忘

- 烧录：P4=COM27，桥=COM28，均 `-b 460800`；IDF 激活：
  `. "C:\Espressif\tools\Microsoft.v6.0.3.PowerShell_profile.ps1"`。
- 串口日志记录：PowerShell SerialPort 115200 ReadLine 循环写临时文件（持有端口，烧录前先停）；
  复位：`DtrEnable=false; RtsEnable=true; 150ms; RtsEnable=false`。
- PC 连热点会抢 `192.168.4.2`（与手机重复 IP）：`ipconfig /release "WLAN"; ipconfig /renew
  "WLAN"` 换到其它地址；测完 `netsh wlan disconnect` 避免干扰手机。
- iPhone 状态修复（当 iOS 拒绝启动会话时）：忘记 BT 设备 + 删 CarPlay 条目 + 重启手机 +
  重新配对；**日志出现 `POST /pair-setup` 才算真的刷新了配对**（只是"重连蓝牙"不产生它）。
- C6 从机固件旧：每次启动警告 `Host [2.12.0] > Co-proc [0.0.0] ==> Upgrade co-proc`；未观察到
  实际 RPC timeout；后续可选方案：刷匹配的 C6 从机固件（Waveshare 提供），或整项目退到
  IDF 5.5 + esp_hosted 1.4.x（工作量大，不推荐）。
- Git 远程：`https://github.com/W2215889498/songplay`（布局 `esp32p4/` + `esp32-bt-bridge/`）；
  最近一次推送 = `6f80714`（集群激活门修复）；**本地增量（9.1 ARP 修复、9.2 800x480/viewAreas/
  裁剪、9.4 日志、桥 rx 日志）尚未推送**。推送方法：用 `%TEMP%\kilo\songplay` 的 clone，
  robocopy 同步（排除 build/managed_components/xcertplay/reference/sdkconfig），commit，push。

### 9.6 待解决问题清单（按优先级）

1. **验证 9.1 的 ARP 修复**：重连出现 `ARP cache purged after station change`；再做"手机换随机
   MAC 后不重启板子也能连"的对照测试。
2. **小分辨率仪表盘落地 + 帧率测量**：已完成（9.7）。当前固件 = 240×400 竖屏
   （实测 ~16-24fps）；320×240、800×480 的数据也在 9.7。换尺寸只改 9.2 所列字段。
3. **重做启动提速**（9.3，逐项上、逐项测）。
4. **渲染优化（帧率的真正来源）**：用 P4 2D-PPA（`esp_driver_ppa`，IDF 6.0.3 有）做
   YUV420→RGB565 硬件转换，替代 CPU 循环（当前 480x800 ≈95ms/帧；320x240 预计 ~25ms，
   加上解码 30ms 可达 ~15-20fps）。
5. **协议补齐**：`/feedback` 返回真正的 plist（stream clock；参照 LIVI `_handle`，音频流
   `{type, sampleRate}`）；event 通道触摸 HID（Events 密钥已派生、命令通道已打通，见
   `xc_airplay_cluster_map_enable` 的模式）；音频接收/解码/播放已就绪（9.8），但
   iPhone 仍不下发音频流——见 9.8 归档与候选清单。
6. **平台卫生**：C6 从机固件升级；桥 RFCOMM 失败槽位泄漏缓解（当前：1..30 扫描、2.5s/通道、
   失败后 200ms 间隙；坏状态时重启桥）。
7. **（可选）降本评估**：C6 是三者中最便宜器件；去掉 C6 需经典 ESP32 用 UART PPP 隧道给 P4
   供网（带宽受限、视频风险大），或评估经典 ESP32 同时承担 Wi-Fi(SDIO esp-hosted)+BT 桥
   （需查 esp-hosted 对经典 ESP32 从机与 BT 的支持与板级走线改动）。

### 9.7 2026-10-08：仪表盘 320×240 落地（已真机验证）

- 改动：
  - `xc_airplay.c` cluster（type 111）800×480 → **320×240**：`dk2/sk2/vk2` 的
    widthPixels/heightPixels 全部改齐；widthPhysical/heightPhysical = **200/150mm**
    （参考默认：宽 200mm、高度按像素纵横比 —— LIVI getInfo.ts / xcertplay
    AirPlayDisplaySettings 一致；LIVI 自己的 ESP32 cluster 例程用的更小：240×240）。
  - `xc_video.c` 新增每 5s 一句帧率统计：
    `ch111 fps x.y (N frames in 5000 ms), decode .. ms, render .. ms`
    （原来只有前 20 帧的逐帧耗时，无法看持续帧率）。
- 真机实测（iPhone iOS 15.7，已有配对的会话；板子复位后 ~18s 完成全链路）：
  `ch111 decoded resolution 320x240` → `cluster map activated` → 持续出帧；
  逐帧 decode ~12ms / render ~20ms（800×480 时代是 ~95-160ms render）；
  实收 ~4fps（20-21 帧/5s）——当前**手机侧只按这个节奏发包**（静态地图场景；
  是否在导航/地图动画影响这个数字，待导航激活时复测）；P4 侧容量 ~30fps。
- 渲染仍居中（流尺寸小于面板时居中显示：320×240 时 x0=80/y0=280；240×400 时
  x0=120/y0=200），中心裁剪逻辑不变。
- 续测 **240×400 竖屏**（当前固件）：`xc_airplay.c` cluster 改 240×400、
  widthPhysical/heightPhysical=200/333mm（同样参考默认公式）；真机 iOS 直接接受
  （`ch111 decoded resolution 240x400`，showUI/forceKeyFrame 正常），
  实测 **~16-24fps**（83-119 帧/5s），decode 13-44ms、render ~25ms —— 是三个
  分辨率里表现最好的（320×240 当时实收 ~4fps，说明帧率主要取决于手机侧节奏/
  地图动画状态，P4 侧余量充足）。若要回头看对比：改回几组数字即可，见 9.2。
- 操作备忘：旧的串口日志进程会让 `idf.py flash` 报 "port busy"，先停掉持有 COM27 的
  powershell 进程再烧（9.5 的 logger 同款问题）。

### 9.8 2026-10-09：导航音频(CarPlay→车机)排查归档（未解决，大量路径已排除）

**背景**：目标是把 iPhone 的导航播报/Siri 声音从板载 ES8311 喇叭放出来。iPhone 却始终
"自己播"——从不下发音频流（SETUP 里没有 type 100/101/102，也没有 duckAudio 命令）。

**已实现并保留（无线 CarPlay 标准行为，有日志实证，建议保留）：**

1. **iAP2 over CarPlay 隧道**（无线 CarPlay 的必经步骤）：
   - `xc_bt_link.c`：收到 `disableBluetooth` 后发起迁移——先发 6 字节 MARKER 再发
     SYN/LSP（**空发 SYN 手机不理，必须先有 MARKER**，这是本次踩的最大坑），链路帧
     经事件通道 `{type:'iAPSendMessage', params:{data}}` 发给手机；手机回包走
     `/command iAPSendMessage` → `xc_link_tunnel_rx()`。8 秒不成自动回退 BT（本项已
     实测：回退后会话完整恢复）。迁移成功后 RFCOMM 由桥断开。
   - `xc_airplay.c`：`event_send_command` 缓冲区 512→4096 + 发送加互斥；
     `xc_plist_get_data()` 新增（读 params.data）。
   - 桥固件（`..\xcertplay-bt-bridge`）：cmd 2 后加 `s_suppress_connect` 闩锁，防止
     自动重连把 iAP2 拉回 BT；cmd 1 解除。
   - 实证：迁移后 identification/MFi/CarPlayStartSession/位置上报全部在隧道重跑，
     视频流不受影响。
2. **认证后 5 条订阅**（LIVI bringup.rs 同款）：0x5000 StartNowPlayingUpdates、
   0x5200 StartRouteGuidanceUpdates、0xAE00 StartPowerUpdates、0x4157
   StartCommunicationsUpdates、0x4154 StartCallStateUpdates（`send_subscriptions()`）。
   实证：手机随即开始推 0x5001/0xAE01/0x4158/0x4155（`state update stream started`）。
3. **音频接收链路（待手机发流即可工作）**：SETUP 100/101/102 应答
   `{type,dataPort,controlPort,streamConnectionID}`（必须回显 id）；UDP RTP 解密
   （AAD=头 4..11，尾部 8B LE nonce）；PCM 16bit BE→LE→立体声播放；Opus 用
   `esp_opus_dec`(48k mono) 解码；ES8311 立体声+open 后设音量；开机自检音（已验证喇叭
   硬件链路 OK）。AAC(102 媒体)未解码。
4. `/info` 音频声明与 LIVI/xcertplay 完全对齐（9 audioFormats/9 audioLatencies、
   Opus 0x70000000 位、features 音频位），并已做字节级核对（9.8 会话里导出解码比对）。

**排除清单**：/info 内容（逐字段核对）、identification 组件、iAP2 全流程（BT+隧道均
正常）、订阅缺失、资源仲裁声明（modes）、Opus/PCM 格式、手机/板子重连与缓存
（"忘记重配"级重连也做过）、喇叭硬件。
**现象线索**：modesChanged 里 audio 资源(resourceID 2)始终不被手机接管（屏幕资源 1
会被接管并投屏）；手机对播报毫无音频路径动作（不 duck、不 SETUP）。

**下一步候选（按怀疑度）**：
1. `/feedback` 返回音频 stream clock（LIVI 用 audioMeta 锚定 `{type,sampleRate,...}`；
   空 200 会让 102 流被反复拆；导航 101 是否需要待验证）。
2. 会话里 `0xA100`(StartVehicleStatusUpdates) 我们仍未响应（LIVI 也只记录）；`0x4E0B`
   (device time) 未管——两者若与音频无关可忽略。
3. HID 细节（媒体/电话 HID 的 descriptor）是否影响 iOS 的音频路由判定。
4. modes 的 take/borrow 交互（配件主动 take audio 资源？）。
5. 手机侧 sysdiagnose/USB 日志（需真人配合复现）。
6. 备选路线：若 iOS 坚持本地播报，评估 BT A2DP 或其它兜底（bridge 目前仅 HID+iAP2）。

**回退/开关**：隧道迁移只在收到 `disableBluetooth` 时触发；要强制回 BT 模式，把
`xc_airplay.c` 里该命令的 `xc_link_enter_tunnel()` 注释掉即可（桥侧 cmd 2 不经 P4 时
不会触发）。桥必须用带闩锁的新固件，否则旧桥会把隧道拉回 BT。

### 9.9 2026-10-09：PPA 硬件色彩转换 + 启动提速重做 + 通话卡片收尾

**PPA 渲染优化（9.6-4）——完成**
- `xc_video.c`：新增 `i420_to_es420()`（openh264 I420 → P4 的打包 YUV420，四字符
  OUYY_EVYY：每 2×2 块 6 字节 = 行0 `U Y Y`、行1 `V Y Y`）+ `ppa_convert()`
  （IDF 6.0.3 API：`ppa_register_client` / `ppa_do_scale_rotate_mirror`；
  `PPA_SRM_COLOR_MODE_YUV420` → `RGB565`，BT601 limited，与旧 CPU 系数一致；
  PSRAM 输入/输出需 128B 对齐——`heap_caps_aligned_alloc(128)`；SRM 驱动内部做
  cache 同步）。CPU 循环保留为 fallback（奇数尺寸或 PPA 失败时）。
- 实测 240×400：decode ~17ms、conv **8-12ms**（旧 CPU 路径 ~20-25ms）、render 总
  **11-16ms**（旧 ~25ms）→ P4 单帧 ~30ms、容量 ~30fps；屏幕帧率仍取决于手机发包
  节奏（静态地图 3-12fps，导航动画时更高）。若颜色异常（发绿/发紫）改 BT709、
  发灰改 FULL range（当前 BT601 limited）。
- 首次帧日志新增 conv 字段：`decode X ms, conv Y ms, render Z ms`。

**启动提速重做（9.3）——完成**
1. `xc_iap2_start()` 提前到 mdns/AirPlay 启动后立即执行（原先排在全部自检后）。
2. 重自检（crypto + 视频彩条）改为 10s 延迟任务：栈 12288、pin core1、
   `xc_airplay_session_started()` 为真则整跳过（日志 `boot self-tests skipped`）。
   **关键修正：MFi 证书读取+缓存（`xc_mfi_selftest()` 实际就是读证书）必须留在
   开机路径**——克隆芯片首次签名后锁证书窗口，且手机最早可能 10s 内请求认证；
   喇叭自检音（`xc_audio_tone_test`）保持开机内联（~5.9s，硬件冒烟）。
3. 桥（..\xcertplay-bt-bridge）：首连延迟 3000→500ms；去掉 SDP 发现 + 1500ms 等待
   （iPhone 的 iAP2 服务是 128-bit UUID，标准 SPP 查询永远查不到）；失败通道后
   200ms 释放 MCB（上一版已有）。
- 结果：日志时间线 AP 4.6s → 证书缓存 5.1s → bridge link 5.6s → 手机 marker 6.1s
  → identification 8.0s → MFi 认证 9.1s → CarPlayStartSession 9.5s → 首帧 ~14.4s；
  用户实测**整机上电→首帧 ≈14s**（此前 ~20s+）。余下 ~8s 为 iPhone 自身进入
  AirPlay 的固定节奏，难以再压。
- 操作注意：烧桥固件后出现过 iPhone 反复 SPP 连上 ~3s 即关闭的僵持状态
  （P4 侧无 marker），手机+板子一起重新连接后恢复；属 iOS 侧状态机怪癖。

**通话卡片（本日早前完成，补充归档）**：iAP2 `0x4155 CallStateUpdate` → 仪表盘地图
顶部深色圆角卡片（LVGL canvas ARGB8888 渲染 + 与视频帧缓冲 alpha 混合，随帧一次上屏
无闪烁）；呼入绿条/呼出蓝条、姓名/号码（**GB2312 全量字库**：`xc_cjk16.c` 由
lv_font_conv + SimHei 16px 生成，6763 汉字，~0.7MB）、通话中计时、挂断 3s 后消失。
微信来电不在电话状态流（无 CallKit）→ 无卡片，属预期。

### 9.10 2026-10-09：目标 <10s 启动 —— 方案与实施结果（A+B 已落地）

当前实测整机上电→首帧 ≈14s。时间账（日志时间线）：
- 1.4s bootloader+早期 init；~2.2s 显示/LVGL 完成开始等 C6；
- 2.1s 释放 C6 复位，**C6 固件启动+SDIO 枚举+握手 2.1→4.4s**（P4 空等约 1.5s）；
- 4.6s softAP 就绪；**5.6-9.5s 桥链+idnf+MFi+StartSession**（原本排在 AP 之后，
  但 iAP2 只依赖 UART，**不需要 AP**——最大可压段）；
- 9.5→14.4s 手机侧：连 Wi-Fi 1.7s + AirPlay 会话/auth 2.2s + 编码预热 ~1s，
  **手机固定开销 ~5s，不可压**。

方案（按收益排序，逐项单独实测）：
- **A（核心）BT 链路+iAP2 与 Wi-Fi/C6 并行启动**：把桥链路、`xc_iap2_start`、MFi
  证书缓存挪到 app_main 最前（只依赖 UART）；`CarPlayStartSession`（ssid/ip/port
  可静态构造）gate 到 AP ready 再发。预期 marker ~3s、认证 ~4s、首帧 ~8.5-10s。
- **B（配套）C6 更早启动**：app_main 第一行即拉起 esp_hosted 传输 / 提前释放
  GPIO54，让 C6 的 1.5s 固件启动与其它 init 重叠；AP ~3-3.5s。esp_hosted 也许要
  本地化打补丁（避免二次复位）。
- **C（收尾）**：identification 去掉必被拒的 0x0016 组件（省一轮）；桥 BT 栈
  init 提速；**C6 从机固件升级**（消除版本不匹配，可能再省 0.3-0.8s）。
- 预测：A+B ≈ 10±1s；A+B+C ≈ 8.5-9.5s。
- 兜底：开机先显示本地仪表盘/品牌界面（"视感画面"<2s）；或常供电+深睡唤醒 <2s。

风险：A 是启动时序重排（显示/网络/iAP2 依赖），需每步日志时间线验证；B 需补丁
esp_hosted（managed → 本地组件）。验证口径：给 P4 复位（或整机上电）+ 抓串口
时间线，记录 AP/ marker/认证/StartSession/首帧五个时间点。

**实施结果（2026-10-09 当天完成 A+B）**：
- A 已实现：`main.c` 把 identity/MFi 证书缓存/`xc_iap2_start` 提到最前（MFi 是
  位敲 I2C，无显示依赖）；`CarPlayStartSession` 加 AP-ready 门控
  （`xc_iap2_notify_ap_ready()`，手机 4s 就来问时挂起、AP 就绪立刻发）。
- B 已实现：`net_task` 独立任务最早期启动——分析确认 **C6 从机启动是由
  `esp_wifi_init()`（即 `xc_net_start_ap`）触发的**，把它挪到 ~1.5s 让 C6 的 1.5s
  固件启动与其余 init 完全重叠；无需补丁 esp_hosted。`xc_callcard_init` 改为懒
  初始化（airplay 栈可能先于显示屏就绪）。
- 实测冷启动：C6 触发 1.63s → marker 1.97s → 认证 3.65s → **AP 3.87s** →
  **StartSession 4.08s** → 手机 7.0s 进 Wi-Fi。P4 侧全部工作在 ~4.2s 完成
  （实施前 ~9.5s）。首帧实测 ~9.5-13s 波动，完全取决于手机（pair-setup 重做、
  Wi-Fi 重连退避等手机侧行为；快速连续插拔会让 iPhone BT 重连指数退避，等
  30-60s 再上电即可）。
- 未做：C（0x0016 参数预剔除 ~0.2s、C6 从机固件升级）。当前瓶颈已全在手机侧
  （Wi-Fi 加入+AirPlay 会话 ~5s，硬底）。
- **用户验收（2026-10-09）：启动速度已满意，本项收尾；后续 C 项与 C6 从机固件
  升级仅作为可选/稳定性事项。**
