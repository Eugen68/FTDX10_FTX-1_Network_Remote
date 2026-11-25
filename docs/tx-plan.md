# Transmit (TX) support: implementation plan

This plan is written for an agent (or a person) who picks the work up cold. Read all of it before you
change anything. Work phase by phase: every phase ends with a build, the host tests, a pull request and a
**hardware test by the owner (Eugene, YO9VA)**. Do not start a phase before the previous one was tested on
the radio and merged.

---

## 0. Context

**What this is.** Firmware for an ESP32-S3 (ESP32-S3-N16R8, 16 MB flash, 8 MB octal PSRAM; the owner's
board is a YD-ESP32-S3 "YD-ESP32-23 V1.3" with the USB-OTG solder bridge closed so its USB port powers
VBUS) that connects to a Yaesu **FTX-1** or **FTDX10** over one USB cable and serves a browser page over
HTTPS: receive audio, CAT control, an FT8 decoder in the browser. Today it is **receive only**, and that is a
documented safety property (README, "Restrictions that keep the radio from transmitting"). This plan adds
transmit in a way that keeps the radio safe.

**Repository:** `Eugen68/FTDX10_FTX-1_Network_Remote`, default branch `main`. Fork of
`pizzamax/FTX-1_Network_Companion` (licence PolyForm Noncommercial 1.0.0; keep `LICENSE` and `NOTICE`).

**Code you will touch**

| File | Role today |
|---|---|
| `main/cat.c`, `main/cat.h` | CAT client. Detects the radio from `ID;` and loads a `rig_profile_t` (`s_profiles[]`). **Every byte sent to the radio passes `cat_cmd_allowed()`**, an allow-list checked against the profile. `TX` is allowed today only in its read form `TX;` (FTDX10). |
| `main/usb_audio.c`, `.h` | USB host + `espressif/usb_host_uac`. Opens the radio's audio **input** only (`UAC_HOST_DRIVER_EVENT_RX_CONNECTED`). USB host task, its interrupt and the UAC task run on **core 1** at priority 20 (`USB_CORE`); do not move them (core 0 has Wi-Fi at 23, and USB audio packets were lost there). **Bus recovery** (`recover_tick()` in `manager_task`): when devices are on the bus but no audio *input* streams for 10 s, the root port is powered off and on (3 tries 15 s apart, then every 2 min); `usb_audio_prepare_restart()` powers it off before REBOOT and OTA restarts. Keep both. |
| `main/acc_scope.c`, `acc_frame.c` | FTDX10 only: reads the radio's scope from the ACC socket as an **SPI slave on SPI2_HOST, GPIO10 / 11 / 13** (GPIO38 / 39 carry a passive link), task on core 1 at priority 12. Do not reuse these pins or SPI2_HOST. |
| `main/web_server.c`, `.h` | HTTPS server. `/stream.pcm` / `/stream.wav` (with `?rate=`), `/cat` GET/POST, `/status`, `/reboot`. Stream: USB reader task (core 1, prio 19) → FIFO in PSRAM → sender task. Strict, allocation-free JSON parser for `POST /cat`. |
| `main/web_auth.c` | Basic auth, lockout, CSRF (`X-Requested-With: ftx1`, `Origin`, `Sec-Fetch-Site`), CSP header. |
| `main/app_config.c`, `main/console_menu.c` | Settings in NVS, serial menu on the UART (COM) port. |
| `main/index.html` | The whole page (embedded). Player with adaptive playout delay, scopes, FT8 decoder in a Web Worker, CAT controls. |
| `test/host_cat/` | Host test of `cat.c` (gcc + stubs). `test/host_cat/run.sh`. |
| `sdkconfig.defaults` | PSRAM, LwIP, UAC transfer counts, etc. Comments explain each choice; keep them. |
| `docs/FTX-1_series_CAT_user_2025.pdf`, `docs/FTDX10_CAT_OM_ENG_2308-F.pdf` | The CAT manuals. **The source of truth for every command and menu number.** Extract text with `pdftotext -layout`. |

**Reference code by the same owner (MIT, may be ported with attribution in `NOTICE`):**
`https://github.com/Eugen68/ftx1-pult` (Python server + browser, already transmits voice / FT8 / CW on an FTX-1):

- `web/worklets.js` → `MicCapture`: browser microphone → 16/8 kHz Float32 → 20 ms frames.
- `web/app.js` around `C.micNode.port.onmessage`: binary frame `[3][rate/8000][int16 LE PCM]` over WebSocket,
  sent only while this client owns PTT and PTT is held; `bufferedAmount < 64000` back-pressure.
- `server/audio.py` → `TxBuffer`: jitter buffer, 80 ms prefill, max 600 ms, ±0.5 % rate nudging for clock
  drift, grows prefill after an underrun.
- `server/main.py` around `ptt_alive` / `max_tx_seconds`: PTT owner, TX dropped after 1.5 s without a
  client heartbeat or after `max_tx_seconds` (default 180).
- `server/ft8.py` → `gfsk()` (FT8/FT4 GFSK synthesis), `MODES` timing table; FT8 encoding through
  `server/ftx/lib` (Karlis Goba's **ft8_lib**, MIT).
- `server/cat.py` → `ptt()`: `TX1;` / `TX2;` / `TX0;`, or RTS on the second port.

**Build and test environment**

- ESP-IDF **v6.1**, target `esp32s3`. In the build environment it is installed at
  `~/esp/esp-idf` by the environment's setup script; `. ~/esp/esp-idf/export.sh`.
- `idf.py build` must finish with **no warnings in `main/`**. `idf.py merge-bin` for a full image.
- `test/host_cat/run.sh` must pass; extend it (see each phase).
- The page: syntax-check the extracted `<script>` blocks with `node --check`, and load the page in Chromium
  (Playwright is preinstalled) against a small Python mock of `/cat`, `/status`, `/stream.pcm` (a mock was
  used during development: it serves the HTML, a JSON `/cat` with the profile fields, and a chunked PCM
  stream honouring `?rate=`). No console errors.
- You cannot reach the radio. The owner tests on hardware and reports `/status`, the serial log
  (115200 bps on the COM port) and the browser console (`audioDiag`).
- Deliver firmware to the owner as `build/network_remote.bin` (renamed `network_remote_ota.bin`) plus
  `build/ota_data_initial.bin`; he flashes them with `tools/scrie_firmware.bat` option 1, which keeps the
  Wi-Fi and passwords. **Never** hand him the merged image for an update: it erases NVS.
- Partitions: `partitions.csv`, two **4 MB** OTA app slots (the app uses about 39 %), NVS at 0x9000, an 8 MB `storage`
  data partition reserved (not used yet; stored voice messages could go there). Do not change the table without a
  reason: moving boards to a new table needs a cable write (`tools/scrie_firmware.bat` option 3).
- Other parts that exist since this plan was first written: the CAT LOG (`GET /catlog`, every command sent, BLOCKED
  ones included; TX commands will show there), `/status` stream statistics (`usb_in_hz`, `usb_bus_resets`, ...), the
  page's AUDIO rate (`?rate=`) and LATENCY (Low / Normal / Safe) selectors, background playback on phones through
  `/stream.wav`, and a plain HTTP server on port 80 that only redirects to HTTPS. LwIP has 24 sockets, HTTPS 10.
- (Superseded) App partition: two 1700 KB OTA slots, about 160 KB free at the time. If a phase does not fit, propose a new
  partition table in the PR (16 MB flash allows much larger slots); changing it needs one serial flash.

**Git workflow.** One branch per phase from up-to-date `main` (`tx-phase-1-ptt`, ...). Small commits with
real messages. Push, open a PR to `main` in `Eugen68/FTDX10_FTX-1_Network_Remote` (via `gh api
repos/Eugen68/FTDX10_FTX-1_Network_Remote/pulls`), list in the PR what was tested and what needs the radio. Do
not merge yourself.

---

## 1. Safety requirements (non-negotiable, every phase)

1. **Off by default.** Transmit is disabled until the owner enables it in the serial menu (new setting
   `tx_enabled`, stored in NVS, default **false**). With it off, the firmware behaves exactly as today:
   `TX1;` can never pass the allow-list, no TX endpoint accepts anything, the page shows no TX controls.
2. **The allow-list stays the only path to the radio.** Add `TX1;` and `TX0;` to `cat_cmd_allowed()` only
   when the profile allows TX **and** `tx_enabled` is set. `TX0;` (receive) may always pass. Never add a
   raw-command path. Extend `test/host_cat` to prove: TX1 blocked when disabled, blocked on the unknown
   profile, allowed only in the enabled case; `TX2;`, `MX`, `KY`, `VX`, `PC` still blocked.
3. **The device ends every transmission by itself.** A TX state machine in firmware (not in the browser)
   owns PTT. It sends `TX0;` and stops the USB audio output when any of these happens:
   - no heartbeat from the owning client for **1.5 s**;
   - the TX time limit is reached (`tx_max_seconds`, NVS, default **120**, range 10–600);
   - the client disconnects, the CAT port is lost, the USB audio device goes away;
   - the radio reports something unexpected (FTX-1 `RI0;` P2 = 1 Hi-SWR; FTDX10: poll `RI0;`, P2 = 1 is
     Hi-SWR — verify in the manual);
   - the firmware boots (always send `TX0;` once CAT is found), reboots, starts an OTA update;
   - **before the USB bus recovery power-cycles the root port**: `recover_tick()` must call the unkey path
     first (or skip the reset while KEYED and let the state machine stop TX on its own). A bus reset in the
     middle of a transmission would drop CAT and the audio output with the radio still keyed.
4. **One transmitter at a time.** The first client that asks gets PTT; others are refused with a clear
   message until it releases. Show the owner in `/status`.
5. **Separate authorisation.** Transmit needs the web login **and** a separate TX password (NVS
   `tx_pass`, ≥ 12 characters, set in the serial menu). Without it set, TX stays off even with
   `tx_enabled`. Check it with the same constant-time compare and lockout as `web_auth.c`.
6. **Visible state.** The page shows a large red ON AIR indicator from the device's state (not from the
   button), the remaining TX time, and why TX ended.
7. **DTR / RTS stay low** on both CP210x ports (already done in `cp210x_setup()`); do not add RTS PTT in
   this plan.
8. **Never claim more than was tested.** Every PR says which parts ran on the radio and which only in a
   build or a mock.
9. **Update the README** security section in the same PR that adds the capability: what can transmit, how
   it is enabled, every automatic stop. Keep "Do not expose it to the Internet".
10. **Use each radio's SWR indicator scale.** Keep the existing FTX-1 and FTDX10 RM6 tables and
    the default 3.0:1 cutoff on those scales.

Owner's hardware test rule for every TX phase: **dummy load, minimum power, the owner at the radio**.

---

## 2. Radio facts (verify against the PDFs before you rely on them)

| | FTX-1 (ID 0840) | FTDX10 (ID 0761) |
|---|---|---|
| PTT by CAT | `TX1;` = CAT TX on, `TX0;` = off, `TX;` read (answer `TX0;` / `TX1;` / `TX2;`, 2 = transmitting from the radio) | same |
| Modulation source, DATA mode | `EX010413` MOD SOURCE: 0 MIC, 1 USB, 2 Bluetooth, 3 AUTO | `EX010415` DATA MOD SOURCE: 0 MIC, 1 REAR; `EX010416` REAR SELECT: 0 DATA, 1 USB |
| USB input level, DATA mode | `EX010414` USB MOD GAIN 000–100 | `EX010417` RPORT GAIN 000–100 |
| SSB mode (voice) | `EX010113` MOD SOURCE, `EX010114` USB MOD GAIN | `EX010113` SSB MOD SOURCE (1 REAR), `EX010114` REAR SELECT (1 USB), `EX010115` RPORT GAIN |
| AM / FM | AM `EX010213/14`; FM `EX010312` MOD SOURCE, `EX010313` USB MOD GAIN | AM `EX010213`, `EX010215`, `EX010216`; FM `EX010312`, `EX010314`, `EX010315` |
| Meters while transmitting | `RM` items 4 ALC, 5 PO, 6 SWR (`RM4;` etc.; check the answer format) | same item numbers |
| RX audio (measured) | UAC 1.0, 48 kHz, 2 ch (L = MAIN, R = SUB), 16 bit | not measured yet |
| TX audio | **unknown: measure in phase 0** | unknown |

Rules for the menu items: **do not write them automatically in this plan.** The owner sets MOD SOURCE and
the gain on the radio. The page may read them (`EXnnnnnn;` read form, add to the allow-list as read-only,
per profile) and warn when the current mode has no USB modulation. On FTX-1 in SSB, AUTO selects USB
when CAT keys the transmitter.

---

## 3. Phases

### Phase 0: find the radio's USB audio output (no transmit)

Build on the current `main` (bus recovery, 4 MB partitions, ACC scope). Opening the output interface must not
disturb the input stream or make `recover_tick()` think audio is missing: it only looks at the input, keep it
that way. Check `stream.usb_in_hz` (about 47,950 or more) and `usb_bus_resets` (0) in `/status` with the output
interface opened, on the FTX-1 and on the FTDX10.

Goal: know whether the radio exposes a UAC **output** (speaker) interface and its formats.

- `usb_audio.c`: handle `UAC_HOST_DRIVER_EVENT_TX_CONNECTED`. Open the interface with
  `uac_host_device_open()`, call `uac_host_printf_device_param()`, store the format list, **do not start
  it**. Keep the RX path unchanged.
- `/status`: add `"tx_audio":{"present":bool,"rates":[...],"channels":n,"bits":n}`.
- Acceptance: build clean; the owner's serial log shows the TX interface parameters for the FTX-1 (and
  later the FTDX10). If no TX interface exists, stop and report: the voice/data TX plan then needs the
  radio's rear DATA jack instead, which is out of scope.

### Phase 1: TX settings, allow-list and the PTT state machine (no audio)

- `app_config`: `tx_enabled` (bool, default false), `tx_pass` (≥ 12 chars), `tx_max_seconds` (default
  120). Serial menu entries for them, shown in the summary like the other secrets (masked).
- `rig_profile_t`: `bool can_tx` (FTX-1 and FTDX10 true, unknown false), `const char *ex_read[]` list of
  menu items the page may read (section 2).
- `cat.c`: allow `TX1;` / `TX0;` per section 1.2; poll `TX;` (both radios) and the Hi-SWR flag while
  transmitting; `cat_state_t` gets `tx_cat` (we keyed it) and `tx_radio` (radio reports TX).
- New `main/tx.c`, `tx.h`: the state machine. States `IDLE`, `ARMED` (owner holds the lock),
  `KEYED`. API: `tx_acquire(owner_id)`, `tx_key(owner_id)`, `tx_heartbeat(owner_id)`, `tx_unkey(reason)`,
  `tx_get_state()`. A 100 ms esp_timer checks heartbeat age and time limit and calls `cat_tx_off()`.
  `tx_unkey()` is idempotent and callable from any task. Every stop records a reason string.
- Transport for control: add a **WebSocket** endpoint `/ws` (`CONFIG_HTTPD_WS_SUPPORT=y` in
  `sdkconfig.defaults`; esp_https_server supports it). Authenticate it like the other endpoints (Basic auth
  on the upgrade request, `Origin` check; the browser sends the CSRF marker as a query or first message
  since WebSocket cannot set headers). Messages (JSON text): `{"t":"tx_auth","pass":"..."}`,
  `{"t":"ptt","on":true}`, `{"t":"hb"}` every 250 ms; the device answers state changes
  `{"t":"tx","state":"KEYED","left_s":117,"reason":""}`. Keep the JSON parsing as strict as `POST /cat`.
  Check that the page's CSP `connect-src 'self'` lets the browser open `wss://` to the same host (it does in
  current Chrome and Firefox; confirm, and add `wss:` for the host only if a browser refuses).
- Page: a TX panel (hidden unless `/cat` reports `tx_enabled`): TX password field (kept in memory only,
  never in localStorage), a **hold-to-talk** PTT button (pointer down = on, up/leave/blur/visibility
  hidden = off), ON AIR indicator, time left, last stop reason. Space bar as PTT only while the page has
  focus, released on `keyup` and on `blur`.
- Host tests: extend `test/host_cat` for the allow-list; add `test/host_tx` for the state machine with a
  fake clock (heartbeat timeout, time limit, second client refused, unkey on CAT loss, idempotent unkey).
- Owner test (dummy load, minimum power, MOD SOURCE as he likes, no audio): PTT keys and unkeys; closing
  the tab, pulling the Wi-Fi (switch the PC's Wi-Fi off) and waiting past the time limit each unkey within
  the stated time; a second browser is refused.

### Phase 2: USB audio output and a test tone

- `usb_audio.c`: start the TX interface (48 kHz, 16-bit if offered; channels as the device reports) only
  while KEYED; `uac_host_device_write()` from a dedicated TX audio task on core 1 (priority 18, below the
  RX reader and the USB tasks). The UAC driver's TX ring buffer: 16 KB. Stop and flush on unkey.
- A tone generator in firmware (sine, 1000 Hz default, level in dBFS, two-tone option 700 + 1900 Hz for
  linearity checks), selectable from the TX panel. Audio only flows while KEYED.
- Make sure RX streaming keeps working while TX audio runs (the radio may mute its RX audio while
  transmitting; that is fine). Watch `stream.usb_in_hz` in `/status`.
- `/status`: `tx.audio_rate`, `tx.underruns`, `tx.written_bytes`.
- Owner test: in DATA-USB with MOD SOURCE = USB, the tone gives ALC/PO on the radio's meter; adjust USB MOD
  GAIN; two-tone looks clean on a receiver if he has one. Report the levels.

### Phase 3: voice from the browser microphone

- Page: port `MicCapture` from ftx1-pult `web/worklets.js` (AudioWorklet, 16 kHz, 20 ms frames, Float32 →
  int16). Group two captures in one binary message over `/ws` only while KEYED and owner:
  `[0x03][rate/8000][int16 LE …]`,
  respecting `bufferedAmount`. Microphone permission is asked only when the TX panel is first used.
  `getUserMedia` constraints: echoCancellation true, noiseSuppression false, autoGainControl false.
- Mute the RX playback in the page while KEYED (feedback), restore after.
- Firmware: a TX jitter buffer in PSRAM modelled on ftx1-pult `TxBuffer` (prefill 80 ms, max 600 ms, grow
  prefill after an underrun, ±0.5 % rate nudge to follow the clock difference between the PC and the
  radio), upsample 16 → 48 kHz with a windowed-sinc FIR (the RX decimator in `web_server.c` shows the
  pattern), write to USB. Silence when empty, never stale audio.
- Heartbeat still comes from the control messages; audio frames do not count as a heartbeat.
- Owner test: SSB into a dummy load, a second receiver or a WebSDR to listen; latency and quality; long
  over to check the time limit.

### Phase 4: FT8 transmit generated on the device

- Time: use SNTP (`esp_netif_sntp`, configurable server, default `pool.ntp.org`) as the primary UTC
  source. If SNTP is unavailable or stale, the active browser may supply UTC over the authenticated
  control channel. Take several timestamped round trips, prefer the shortest samples and estimate
  one-way delay from half the round-trip time. Treat browser clock accuracy and network asymmetry as
  uncertainties. Show the source, sample age, offset estimate and RTT in `/status`. Inhibit FT8 TX
  when neither source is fresh enough for slot timing.
- Vendor **ft8_lib** by Karlis Goba (MIT, `https://github.com/kgoba/ft8_lib`, same copy as ftx1-pult
  `server/ftx/lib`) as `components/ft8_lib`. Include its upstream `LICENSE` and update the existing
  MIT attribution in `NOTICE` to identify the vendored TX component. Use its message packing and
  `ft8_encode()` to get the 79 tones.
- GFSK synthesis on the device, ported from ftx1-pult `server/ft8.py` `gfsk()` (BT 2.0, 160 ms symbols,
  raised cosine ramps), 48 kHz into PSRAM (12.64 s ≈ 1.2 MB at 16-bit mono; duplicate to stereo when
  writing if the device wants two channels).
- Scheduler: use the selected even/odd 15 s UTC slot. Prepare USB audio and key PTT before the first
  symbol so RF audio starts at **slot +0.5 s**. Measure the USB playback delay on each radio and
  compensate by submitting samples early. Include the FTX-1 sequence that pauses USB RX before
  opening USB TX, and restores RX after confirmed unkey. If setup or key confirmation misses the
  deadline, wait for the next eligible slot so the whole frame is transmitted. Unkey after the last
  symbol has played.
- Require the owning page to be active (visible and focused) with a fresh control heartbeat to arm
  and keep transmitting. Disarm and stop on page blur, hide, disconnect, heartbeat loss or a Phase 1
  safety trip. Cap a single arming at **three consecutive FT8 transmissions** initially, then require
  explicit re-arming.
- Page: in the FT8 panel, a TX row for message text (validated by the packer on the device), audio
  frequency (default 1500 Hz, 200–3000), even/odd, “Enable TX” and “Halt”. Clicking a decoded CQ
  fills a standard reply. No auto-sequencing in this phase. The page's `ft8DtCorr` only affects
  display. TX timing uses the selected UTC source on the device.
- Owner test: use a dummy load and decode a CQ in WSJT-X on both radios. Verify DT within ±0.2 s,
  USB delay compensation and return to RX, SNTP loss and browser fallback under network delay, and
  disarming on page inactivity, lost heartbeat and the consecutive TX cap.

### Phase 5 (optional, later): FT8 auto-sequencing, logging (ADIF), FT4.

---

## 4. Documentation and finishing

- README (English): a "Transmit" section per phase, the safety section rewritten (section 1 of this plan),
  the serial menu entries, the radio menu settings the owner must make (section 2). Note that
  `README.ja.md` is not updated.
- `NOTICE`: ft8_lib (MIT), ftx1-pult (MIT, Eugene Trifu YO9VA) for the ported parts.
- `build_info.h` `FTX1_VERSION`: bump per phase (`1.9.0-yo9va` for phase 1, …).
- Commit trailer lines as the session asks; PR descriptions end with the session's attribution.

## 5. Definition of done (per phase)

- [ ] `idf.py build` clean (no warnings in `main/`), image fits the OTA slot.
- [ ] `test/host_cat/run.sh` (and new host tests) pass.
- [ ] Page loads in Chromium against the mock with no console errors; TX panel hidden when TX is disabled.
- [ ] With `tx_enabled` off: no way to key the radio (proved by the host test and by reading the code paths).
- [ ] PR open with what ran where; firmware files handed to the owner.
- [ ] Owner's hardware test passed and reported; only then the next phase.
