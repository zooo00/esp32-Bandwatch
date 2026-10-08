# Bandwatch developer guide

This is the reference for anyone (human or AI) continuing the project on a fresh machine. It covers the
hardware, the toolchain, the firmware architecture, the serial protocol, the host tool, the known quirks, and
where the authoritative specifications live.

## 1. Hardware

| Item | Value | Source |
| --- | --- | --- |
| Board | Waveshare **ESP32-C5-LCD-1.47** | [product page](https://www.waveshare.com/esp32-c5-lcd-1.47.htm), [wiki/docs](https://docs.waveshare.com/ESP32-C5-LCD-1.47), [official repo with schematic + BSP](https://github.com/waveshareteam/esp32-c5-lcd-1.47) (`HARDWARE_REFERENCE.md`) |
| SoC | ESP32-C5FH4: RISC-V 240 MHz, 384 KB SRAM (≈320 KB usable), 4 MB flash, no PSRAM, Wi‑Fi 6 dual-band (2.4 + 5 GHz), Bluetooth LE 5, IEEE 802.15.4 | [ESP32-C5 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf), [ESP32-C5 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-c5_technical_reference_manual_en.pdf), [hardware design guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c5/) |
| Display | 1.47" IPS, ST7789, 172×320, RGB565, SPI | ST7789 init sequence in `Display_ST7789.cpp` (from Waveshare's C6 demo) |
| LED | 1× WS2812B on **GPIO8** | Arduino core `rgbLedWrite()` (RMT) |
| Buttons | BOOT on **GPIO28** (strapping pin; used as input after boot), RESET | |
| microSD | SPI, CS **GPIO4** (shares SCLK/MOSI/MISO with the LCD; pcap recording §12, event log §20) | |

Display pins: SCLK **7**, MOSI **6**, MISO **5**, LCD CS **23**, DC **24**, RST **26**, backlight **10** (LEDC PWM).
Panel gap X = 34, Y = 0. SPI clock 40 MHz (the SPI2 source clock on the C5 is 40 MHz; the upstream "80 MHz"
was silently capped).

Strapping pins on the C5 (from `soc/gpio_reg.h` `GPIO_STRAP_REG`): bit1 GPIO2, bit2 GPIO3, bit3 GPIO27,
bit4 GPIO28, bit5 GPIO7. `boot:0x2e` = GPIO28 low = download mode; `boot:0x3e` = normal SPI flash boot. GPIO7
(LCD clock) is a strap but only selects the JTAG source; it does not affect boot mode.

USB: the board's USB-C goes to the chip's native **USB-Serial-JTAG** (no external bridge). Port on macOS:
`/dev/cu.usbmodem*`. Consequences are in section 7.

## 2. Toolchain

- **arduino-cli** ≥ 1.5 with the `esp32:esp32` core **3.3.11** (bundles prebuilt ESP-IDF **5.5.5** libraries;
  ESP32-C5 support arrived in core 3.3.0). Board `esp32:esp32:esp32c5` ("ESP32C5 Dev Module").
- FQBN used by `build.sh`: `esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=4M,PSRAM=disabled,UploadSpeed=460800`
  (3 MB app partition, no OTA; USB CDC console).
- Libraries: **lvgl 9.3.0** (Arduino library manager). LVGL config = `bandwatch/lv_conf.h`, injected with
  `-DLV_CONF_PATH="<abs path>"` through `compiler.cpp.extra_flags` so the library and the sketch share one config.
  The Arduino **BLE** library (NimBLE backend) ships with the core. The 802.15.4 driver (`esp_ieee802154.h`,
  `libieee802154.a`) ships in the prebuilt IDF libs.
- Host: Python 3 + `pyserial`. No other dependencies. On a Homebrew Python (PEP 668 "externally managed")
  a plain `pip install --user` is refused and there is no brew formula, so `setup.sh` falls back to
  `--user --break-system-packages`.
- `setup.sh` installs all of the above on macOS (Homebrew) or a Linux box with arduino-cli on PATH. The core
  version is **pinned** there (`ESP32_CORE_VERSION`, default 3.3.11) because of the deauth offsets in §9.
  On arm64 it also installs universal-ctags unconditionally: `build.sh` always routes ctags through
  `tools/ctags/ctags`, which fails if universal-ctags is missing, whether or not Rosetta is available.
- Reference build (core 3.3.11, lvgl 9.3.0): at **v1.6** it was 0 errors, 0 warnings, 1 880 889 B flash (59 % of
  the 3 MB app partition; the v1.12 boot photos took it to ~2.77 MB) and 79 592 B static RAM. Static RAM is now
  **77,184 B** (v1.19.3; 77,392 B at v1.19), under 3 kB below the ~80 KB line in `CLAUDE.md` rule 4, and `tests/firmware` gates it
  against `tests/firmware/static_ram_ceiling.json`. Watch it when adding globals; §16 has the history and §17 how
  `apSuffix` was added without moving it at all.
- The core's `sdkconfig` is fixed (prebuilt). Relevant values: `CONFIG_SOC_WIFI_SUPPORT_5G=y`,
  `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BT_NIMBLE_EXT_ADV` **not set** (no BLE 5 extended advertising),
  `CONFIG_IEEE802154_ENABLED=y`, `CONFIG_IEEE802154_RX_BUFFER_SIZE=20`, `CONFIG_SPIRAM=y` (harmless
  "PSRAM init failed" at boot; board has none). To change any of this, the project would have to move to ESP-IDF
  (the upstream was Arduino, so it stayed Arduino).

Reference docs for the APIs used:
- Wi‑Fi driver (5 GHz band mode, country/channel mask, promiscuous mode):
  <https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c5/api-reference/network/esp_wifi.html> and the headers
  in `~/Library/Arduino15/packages/esp32/tools/esp32c5-libs/3.3.11/include/esp_wifi/include/` (`esp_wifi.h`,
  `esp_wifi_types_generic.h`, `esp_wifi_he_types.h` for the C5 `wifi_pkt_rx_ctrl_t` layout).
- BLE (Arduino BLE library over NimBLE): `~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.11/libraries/BLE/src/`.
- IEEE 802.15.4 driver: <https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c5/api-reference/network/esp_ieee802154.html>
  (`esp_ieee802154.h`: callbacks run in ISR context; received buffer = `len | MHR | payload`, last 2 bytes are
  RSSI/LQI instead of FCS).
- esptool / boot modes / USB-Serial-JTAG reset behaviour: <https://docs.espressif.com/projects/esptool/en/latest/esp32c5/troubleshooting.html>
  and <https://docs.espressif.com/projects/esptool/en/latest/esp32c5/advanced-topics/boot-mode-selection.html>.
- Arduino-ESP32 core docs: <https://docs.espressif.com/projects/arduino-esp32/en/latest/>.
- Upstream project this was ported from: <https://github.com/PierreGode/WaveshareESP32C6LCD> (bandwatch + blewatch, ESP32-C6).
- LVGL 9 docs: <https://docs.lvgl.io/9.3/>.
- pcap link types: radiotap (127) <https://www.radiotap.org/>, IEEE 802.15.4 TAP (283) <https://github.com/jkcko/ieee802.15.4-tap>.

## 3. Firmware architecture (`bandwatch/`)

Modules behind one shared header: `bandwatch_core.h` holds the tunables, mode/channel model and every state
type (plus extern) that more than one file touches — everything here that lands in DRAM is small on purpose
(§16). Scope rule per file: symbols declared in core.h are defined at **global** scope in exactly one file;
file-local helpers live in an anonymous namespace.

- `bandwatch_core.h` — dwell (220 ms) and other tunables, `kChannels[]` / `kChanBand[]` (13 × 2.4 GHz,
  25 × 5 GHz, 16 × 802.15.4), `BandMode` enum (`5g`, `2.4g`, `both`, `ble`, `154`, `spec`; `kBandModes` = 6), capture ring limits, LCD
  page enum; shared structs (`Accum`, `ChannelState`, `CapFrame`, `Hunt`, `Deauth`, `BleState`, `SdSink`,
  `DevSnap`) and state externs; `serialRoom()` / `sanitizeText()` / `putLE16/32`; cross-module decls.
- `bandwatch.cpp` — the core: channel control (`applyChannelIdx()`, `advanceChannel()` skipping
  disabled/rejected channels, honouring `parkedIdx`; `setBandMode()` is the only place radios are swapped),
  dwell scoring (`computeBusyScore()` log-scaled pkt/s, B/s, strong ratio, unique talkers; smaller references in
  802.15.4 mode; EMA α = 0.22, `globalActivityMax()`, `sortTop3()`), device-table snapshots (copy under lock,
  sort outside), hunt orchestration (`startHunt/startHuntSsid/stopHunt/lookupHuntLabel`, and `serviceHuntSsid()`,
  the SSID hunt's park, §23), and the entry points —
  `Bandwatch_Init()` (called from `Lvgl_Init` in `LVGL_Driver.cpp`) and `Bandwatch_Loop()` (from `loop()`):
  serial commands, button, capture drain, BLE cycle, periodic device/status lines. The per-channel `ChannelState`,
  the shared `Accum` written by the RX paths under `g_accumMux`, and the mode flags live here too.
- `wifi_sniff.cpp` — Wi‑Fi driver lifecycle: `startWifi()` (country with manual policy + full 5 GHz mask, band
  mode, protocols incl. 11ax, explicit raw-TX rate/power, promiscuous filter/callback), `stopWifi()`. Plus the
  RX path in the Wi‑Fi task: `promiscuousCb()` counts frames/bytes/strong, hashes the transmitter for the
  "unique talkers" estimate, calls `trackWifiDevice()` which updates the device table and, for beacons/probe
  responses, runs `parseBeaconIes()` (SSID, RSN/WPA → security bits, HT/VHT/HE/EHT → PHY bits, HT/VHT operation
  → width, BSS load, country). Optionally copies each frame into the capture ring.
- `ble_scan.cpp` — drives NimBLE's `ble_gap_disc()` directly instead of the Arduino `BLEScan` wrapper: one
  callback per on-air advertising report (`bleGapEvent()`) with its own data/address/RSSI/PDU type, a minimal
  AD parser and the pcap LL-frame builder (§13). Discovery runs continuously with no result cache;
  `serviceBle()` only restarts it if the host ends discovery, and applies the passive/active/auto policy.
  Owns the `bleScan` (`BleState`) instance. Runs in the NimBLE host task.
- `ieee154.cpp` — `esp_ieee802154_receive_done()` (ISR): parses the MAC header (frame type, addressing modes,
  PAN id, short/extended source), aux security header, beacon superframe/GTS/pending fields, classifies the
  upper layer (`classify154`: Zigbee NWK version 2, Green Power version 3, 6LoWPAN dispatch bytes, MAC-level
  security ⇒ Thread-style), then `track154()`. The callback needs C linkage, which is why the file-local
  namespace closes before it. `start154()/stop154()` + the `r154Running` instance.
- `deauth_diag.cpp` — the deauth attack and its diagnostics: the `Deauth` instance plus txtest state,
  the driver-internal `libnet80211.a` declarations (§9), `startDeauth()/stopDeauth()/serviceDeauth()` and the
  two TX paths (`sendInternalKick()` via the driver's own frame builder; raw-TX `sendKickFrame()` fallback).
- `capture.cpp` — the capture ring: `ensureCapRing()` (sized against free heap, with a post-allocation floor —
  §16), `releaseCapture()`, single-producer reserve/commit (`capReserve/capCommit`), base64 streaming and the
  per-loop bounded drain.
- `sd_sink.cpp` — microSD pcap sink: on-demand FATFS mount (§12), byte-compatible writer, `sdls` / `sdread` / `sdrm`;
  since 1.18 also card presence (`sdSetPresent()`, the one place it changes; the idle CMD0 probe
  `sdServicePresence()`) and the one I/O-failure path for a card pulled mid-capture (§12).
- `events.cpp` — the C4 event log (1.18, §20): `eventFlag()` (radio side, under `g_devMux`), `serviceEvents()`
  (loop: classify, buffer, flush to `/events.csv`), the `/seen.csv` device register (row logic in `seen_row.h`), and `loadSurvExtra()`
  (`/surveil.csv` extra OUIs, read at boot).
- `settings.cpp` — NVS persistence (C3, 1.11; the `events` flag joined in 1.18, `alerts` in 1.19).
- `led_alert.cpp` — LED alert blips (1.19, §21): `ledNoteSurv()` (radio side, under `g_devMux`), `g_ledJoinFlag`
  (802.15.4 ISR), `serviceLedAlerts()` (loop: dedup, coalesce, rate-limit, draw the pattern).
- `host_proto.cpp` — the serial JSON protocol: `sendHello/sendDwell/sendSweep/sendBleStatus/sendDevices`,
  `handleCommand()` / `pollSerial()`. All output goes through `serialRoom()` so a line is either written whole
  or skipped.
- `lcd_ui.cpp` — LVGL 9, 172×320 portrait: `showPage()` deletes the current page's widgets and builds the new
  one (`buildOverviewPage`, `buildChannelsPage`, `buildSpectrumPage`, `buildDevicesPage`, `buildHuntPage`,
  `buildSystemPage`; `pageAvailable()` hides the pages a mode has no use for);
  `refreshUi()` updates only the visible page and drives the LED (`driveLed()`, which yields to an alert blip, §21). `pollButton()` = tap → next page, hold → walk the
  mode cards every 0.7 s (then a boot-photo stop after Spectrum), release picks (on the Hunt page the hold stops the
  hunt first).
- `devices.h` — device-table structs + open-addressing hash; `surv_ouis.h` — the surveillance-OUI table
  (matched in firmware so the LCD can flag without a host); `Display_ST7789.*`, `LVGL_Driver.*`, `lv_conf.h`
  — display glue.

Tasks and contexts: Arduino `loop` task (LVGL + everything in `Bandwatch_Loop`), Wi‑Fi task (promiscuous
callback), NimBLE host task (advert callback), 802.15.4 ISR. Shared data is protected with the two spinlocks;
never call anything that allocates or blocks inside them.

Memory budget: see §16 for current measured figures (static 77,184 B at v1.19.3; free heap ~99–106 kB in `both`
depending on the LCD page, measured in v1.15.5; BLE ~88 kB, measured in v1.15.2); capture ring allocated on `cap 1` (up to 20 slots: ~1.6 KB each in Wi‑Fi modes, 128 B payload in BLE/802.15.4
since v1.19.3, §16).

## 4. Serial protocol (USB CDC, 115200, newline-delimited)

Device → host, one JSON object per line unless noted:

| Line | When | Fields |
| --- | --- | --- |
| `{"t":"hello",...}` | boot, `info`, after `band` | `fw`, `ver`, `dwell_ms`, `spec_step` (fine-spectrum step in MHz), `band` (mode), `country/bandmode/proto/promisc` (esp_err names), `chs` (channel list of the mode), `park`, `cap`, `snap`, `heap`, `up` (s), `rst` (reset reason), `hunt` (id or null), `h` (hunt status `[rssi, age_ms, hits]` or null), `deauth` (`[bssid, park ch (0 if hopping), frames sent, frames failed]` or null), `sd` (`{mounted, mb, cap, file, frames, bytes, err, clock}` — `mounted` is historical naming: it reports `sd.cardPresent`, *a card is in the slot*, not that FATFS is mounted; since 1.18 it follows removal/insertion, §12), `mir` (§19), `ev` (event-log status, 1.18 — see the `ev` row), `alerts` (LED alert blips on/off, 1.19, §21); for an SSID hunt the escaped name, followed by `ssid` with the same name — C7, v1.20, §23, `pt` (C5 patrol, 1.20: `null` when idle, else `{leg, left, cyc, legs}` — see the `pt` row) |
| `{"t":"d",...}` | every completed dwell | `c` channel, `s` EMA score, `r` raw score, `f` frames, `b` bytes, `st` strong, `u` unique, `g` global max, `n` sweep no., `park`, `cap`, `drop` (capture drops), `da` (deauth frames sent so far; 0 when idle), `df` (deauth frames failed so far; 0 when idle), `sdc` (1 while recording to microSD), `sdf`/`sdb` (frames/bytes written to the card), `h` |
| `{"t":"s",...}` | after every full sweep | `n`, `g`, `band`, `ch`: `[[ch, ema, frames, bytes, strong, unique, state], ...]` (state 0 ok / 1 no data / 2 rejected), `aps`, `drop`, `heap` |
| `{"t":"w","dev":[...]}` | every 2 s in Wi‑Fi modes, **in chunks of ≤24 rows** (v1.17; loudest first, each chunk a complete line; the host merges by MAC, so a chunk dropped for TX room just waits a cycle) | rows `[mac, rssi, max, frames, age_ms, ch, flags, ssid, sec, pmf, phy, bw, util, stations, cc, surv, apSuffix]`; `apSuffix` = last 3 bytes of the BSSID this device was heard associated with, lower-case hex, `""` if never seen on a BSS (§17); flags bit0 AP, bit1 IEs parsed, **bit2 seen only as a frame destination (tier 1)**; `surv` = surveillance category id (0 none); `sec` bits: 0x01 WEP, 0x02 WPA, 0x04 WPA2‑PSK, 0x08 WPA2‑Ent, 0x10 WPA3‑SAE, 0x20 WPA3‑Ent, 0x40 OWE, 0x80 open; `pmf` 0/1/2; `phy` bits 1 legacy, 2 n, 4 ac, 8 ax, 16 be; `bw` in 10 MHz units; `util` 0–255; `cc` country |
| `{"t":"b","dev":[...]}` | every 2 s in BLE mode | rows `[mac, rssi, max, adverts, age_ms, addrType, company, name, appearance, txPower(127=none), svcUuid16, svcDataUuid16, appleType, flags, surv]`; flags bit0 connectable, bit1 legacy adv, bit2 scannable |
| `{"t":"z","dev":[...]}` | every 2 s in 802.15.4 mode | rows `[id, rssi, max, frames, age_ms, ch, pan, short, proto, flags, lqi]`; `id` = extended address `aa:bb:cc:dd:ee:ff:00:11` or `pan/short` hex; `proto` 0 unknown, 1 Zigbee, 2 Zigbee GP, 3 Thread/6LoWPAN, 4 MAC‑secured; flags bit0 ext addr, bit1 beacons, bit2 permit join, bit3 MAC security, bit4 data seen |
| `{"t":"pr",...}` | a directed probe request (Wi‑Fi modes, C1) | `mac` (the probing client; often a randomized, locally administered MAC), `rssi`, `ch`, `ssid` (the network it asked for, control-stripped). Wildcard probes (empty SSID) are not sent. The device suppresses a repeat of the same (MAC, SSID) pair for 60 s (32-entry table in `host_proto.cpp`); the host keeps history and expires pairs after 15 min (`PROBE_TTL_S`), grouping by SSID because phones randomize per burst |
| `{"t":"ble",...}` | every 1 s in BLE mode | `devs`, `cycles`, `heap`, `adv` (advertising reports), `scan` (policy) / `running` (what is actually running) / `switches`, `cap`, `drop`, `sdc`/`sdf`/`sdb`, `h`. **BLE mode emits no dwell lines, so this is the only live capture telemetry there** - anything added to `{"t":"d"}` for the dashboard has to be added here too |
| `{"t":"ev","ev":{...}}` | every 5 s while the event log is armed, in every mode (`sendEventStatus()`, 1.18) | `ev` = `{on, card, base, file, written, pending, surv, new, drop, err, wait}`: armed; card usable (last attach worked); baseline hashes in RAM; device rows in `/seen.csv` (counted at load, plus this session's appends); rows written to `/events.csv`; rows buffered; surveillance rows; new-device rows; rows dropped (2 KB buffer full, rows discarded by `events 0` on a busy card, or the 16-entry radio queue full); card errors (failed mount/write); novelty checks skipped for want of a baseline. The same object rides on `hello` and the `events` ack. §20 |
| `{"t":"pt","pt":{...}\|null}` | every 2 s while patrolling, in every mode (BLE has no dwells), right after each leg hand-off, and once with `null` when a patrol ends (`servicePatrol()`, C5, 1.20) | `pt` = `{"leg": i, "left": ms left in the leg (0 = due, waiting for the dwell boundary), "cyc": full cycles done, "legs": [[mode, sec], ...]}` - at most 127 B (`kPatrolJsonMax`). The same member rides on `hello` (which every hand-off also sends, so the host gets the new mode's `chs`) and the `patrol` ack. §22 |
| `{"t":"ack",...}` / `{"t":"log","msg"}` / `{"t":"err","msg"}` | command replies and notices | Since 1.18: `{"t":"log","msg":"sd card removed[: why]"}` / `"sd card inserted"` on every presence change after boot (`why` e.g. `recording stopped`, `file pull stopped`, `event log buffering`) — the host sets `sd.mounted` from it and drops its stale file list; `{"t":"err","msg":"sdcap: write failed after N frames (card removed?) - recording stopped"}` (host clears `cap`); `{"t":"err","msg":"sdread: read failed at X of Y bytes (card removed?)"}` *instead of* `sdread_done` when a pull comes up short, so the host discards the partial file rather than saving it as complete. Since the C4 follow-up: `{"t":"ack","cmd":"sdrm","file":"/x","ok":1}` after a delete, `"ok":0,"msg":"no such file"|"remove failed"` when the card refused it; refusals before the card is touched are `{"t":"err","msg":"sdrm: ..."}` (`bad file name ...`, `busy - a file is being pulled (sdread)`, `/x is being recorded - stop sdcap first`, `busy - the event log is writing to the card`, `no card`). `{"t":"log","msg":"seen.csv rotated: N -> M in R ms (attach A ms)"}` when an attach trims `/seen.csv`, `"seen.csv converted to v2: N rows in M ms"` when it upgrades a v1 file (§20) |
| `M <x> <y> <w> <h> <base64>` / `MF <seq> <complete>` | while `mirror 1` | one repainted LCD slice (RGB565-LE, <= ~2 KB base64) / end of one LVGL refresh; `complete` 1 = nothing dropped and no repair pending (§19) |
| `S <n> <base64>` | after `sdread <path>` | one chunk of a file being streamed off the card; bracketed by `sdread` / `sdread_done` acks |
| `{"t":"sdls","files":[[name, bytes], ...],"total":N,"sent":M}` | after `sdls` | files in the card root; `sent < total` = the serial buffer filled mid-list (host slow or absent) and whole entries were dropped |
| `P <ch> <rssi> <ts_us> <len> <base64>` | while `cap 1` | one captured frame; `len` = original length, payload may be truncated to the snap length. Wi‑Fi frames include the FCS; 802.15.4 frames exclude it |

Host → device commands: `band 5g|2.4g|both|ble|154|spec`, `park <ch>` / `park 0`, `cap 1|0`, `snap <32..1600>`,
`hunt <mac> [ch]` / `hunt <ext addr>` / `hunt <pan>/<short>` / `hunt 0` (the 8-byte 802.15.4 extended address
works since v1.19.3: `parseMac` used to accept its first 6 bytes as a Wi‑Fi MAC; a key hunt parks only in 802.15.4
mode; ack `{"t":"ack","cmd":"hunt","hunt":"<id>"|null,"park":N}`),
`huntssid <name>` / `huntssid 0` (C7, v1.20, §23: hunt every AP beaconing that exact name - the rest of the line,
spaces included, 1..32 bytes, case-sensitive; acked with the hunt ack plus `"ssid"`:
`{"t":"ack","cmd":"hunt","hunt":"<name>","ssid":"<name>","park":N}`, both JSON-escaped; a longer name is
`{"t":"err","msg":"huntssid: name longer than 32 bytes"}` and leaves the running hunt alone),
`deauth <bssid>` (Wi‑Fi modes only — broadcast deauth to all clients of that AP; stops itself after `kDeauthMaxMs`,
5 min) / `deauth 0`, 
`dca <client_mac> <ap_bssid>` (targeted deauth to one specific station — both MACs colon-separated, the
device rejects anything else) / `dca 0`, 
`sdcap 0|1`, `sdinfo` (mounts, replies, and since 1.18 unmounts again), `sdls`, `sdread <path>`, `time <epoch>`, `info`, `reboot`.
1.18: `events 1|0` (arm/disarm the SD event log, §20; persisted in NVS; arming works with no card; ack
`{"t":"ack","cmd":"events","ev":{...}}`, or `{"t":"err","msg":"events: not enough free heap"}` when its ~12.6 KB
will not allocate), `sdprobe` (diagnostic: `{"t":"ack","cmd":"sdprobe","r1":N,"present":0|1}` — the raw CMD0
reply of the presence probe; `r1` is -1 when the card is mounted or busy and the probe was not sent),
`sdface 0|1` (diagnostic: show the card-out / card-in LCD face for 3 s without touching the card). `txkick`
(1.20, diagnostic for B4: flush the USB TX FIFO and re-arm its interrupt, what `serviceSerialTx()` does on its own;
replies `{"t":"ack","cmd":"txkick","txk":N}`). hello carries `txk`: how many times a kick got a stalled TX moving.
`seengen <n>` (diagnostic, §20: replace `/seen.csv` with n = 1..10000 synthetic register rows, parking the real one in
`/seen.bak.csv`; ack `{"t":"ack","cmd":"seengen","n":N,"ms":M,"ok":0|1}`; `seengen 0` restores; refusals are
`{"t":"err","msg":"seengen: ..."}`).
`sdrm <name>` deletes one file in the card root (§12 "Deleting card files"): `<name>` is `/x` or `x`, 1-39 chars of
`[A-Za-z0-9._-]`, no `..`; ack `{"t":"ack","cmd":"sdrm","file":"/x","ok":1}` (or `"ok":0,"msg":...`), refusals as
`sdrm: ...` err lines (table above). The host re-sends `sdls` after a delete.

1.19: `alerts 1|0` (LED alert blips, §21; default on, persisted in NVS; ack `{"t":"ack","cmd":"alerts","alerts":N}`),
`ledtest surv|new|join` (diagnostic: draw one blip now, bypassing the rate limit and the `alerts` switch but never
over an active deauth; ack `{"t":"ack","cmd":"ledtest","kind":"surv","shown":0|1}` - `shown` 0 means a deauth owns
the LED; any other kind gives `{"t":"err","msg":"ledtest: surv|new|join"}`).

1.20 (C5, §22): `patrol 1` walks the default legs `spec:30,both:40,ble:20` round-robin; `patrol <mode>:<sec>[,...]`
sets 2-6 custom legs (mode `5g|2.4g|both|ble|154|spec`, 5-600 s each) and starts (a running patrol restarts);
`patrol 0` stops and stays in the current mode; bare `patrol` queries. Ack `{"t":"ack","cmd":"patrol","pt":...}`.
Refusals: `{"t":"err","msg":"patrol: legs are 2-6 x mode:sec (...)"}`, `"patrol: stop capture first"` (USB or SD),
`"patrol: stop hunt first"`, `"patrol: stop deauth first"`. While it walks, `cap 1`, `sdcap 1`, `hunt <id>`,
`huntssid <...>`, `deauth <bssid>` and `dca <...>` answer `{"t":"err","msg":"<cmd>: stop patrol first"}` (their stop
forms pass); any `band` (and a BOOT-hold mode walk) stops it first (`{"t":"log","msg":"patrol stopped by band"}`).
Also 1.20: the command line buffer is 64 bytes (a 6-leg `patrol` is 60), and a longer line is refused whole with
`{"t":"err","msg":"line too long"}` instead of running its first 47 characters.

Settings and views (ack `{"t":"ack","cmd":...}`): `specstep 1|2|5` (fine-spectrum step in MHz, spec mode; persisted),
`blescan passive|active|auto` (BLE scan policy, §13; persisted; frozen while recording), `addr1 0|1` (tier-1
destination-only Wi‑Fi tracking, §15; persisted), `mirror 0|1` (LCD mirror, §19; not persisted), `page next|prev`
(step the LCD like a BOOT tap, §19). Deauth diagnostics (§9, §11): `kickpath 0|1`, `kickfc <hex>`, `txtest 0|1|2`,
`txstat`.

The device drops a whole line rather than truncating it, so the host must tolerate missing lines — but it must
also tolerate *malformed* ones: `handle_line()` wraps the dispatch so a short or unexpected line is logged
instead of killing the reader thread (an exception there closes the serial port and silently ends a capture).

## 5. Host tool (`host/`)

`bandwatch_host.py`: a reader thread parses lines into a state dict (channels, history, device tables with
per-device RSSI history, hunt state), an HTTP server exposes the API below, and `PcapWriter` writes radiotap pcaps
for Wi‑Fi, 802.15.4‑TAP pcaps for 802.15.4 and BLE LL pcaps for BLE (§13). Files are named `bandwatch-wifi-YYYYmmdd-HHMMSS.pcap` /
`bandwatch-802154-…` in `--captures` (default `./captures`).

`captures/` is **git-ignored** — it holds your recorded pcaps, not source, so it is the one thing a fresh
`git clone` on another machine does not reproduce. Everything else regenerates (`build/` from `./build.sh`,
the toolchain from `./setup.sh`, `~/.cache/bandwatch/oui.csv` on first host run); the recordings do not. Copy
them by hand if you want them on the other machine.

HTTP API:

- `GET /api/state` — everything, JSON. Wi‑Fi rows with `dest_only` (tier 1, seen only as a frame destination,
  §15) carry `rssi`/`max` as `null` since v1.19.3: the device has no RSSI of their own (the frame's RSSI is the
  sender's), and the dashboard shows "–" for them. During an SSID hunt (C7, §23) `hunt` also carries `ssid` and
  `aps` (every AP in the table beaconing that name, loudest first: `mac`, `ch`, `rssi`, `age`, `vendor`), and its
  `mac` is the loudest one heard in the last 60 s (`""` if none).
- `GET /api/screen` (mirror status) and `GET /screen.bin` (raw RGB565-LE framebuffer, 172×320; §19).
- `GET /file?name=<basename>` — a file in `--captures` (an `sdread` pull or a host pcap) as a download; plain
  basenames only, anything else is 404.
- `POST /api/cmd` — a JSON object `{"cmd": ..., ...}`. Commands: `band` (`value` 5g|2.4g|both|ble|154|spec),
  `specstep` (`value` 1|2|5), `park` (`value` = channel, 0 = hop), `capture` (truthy `value` starts, optional `snaplen`), `hunt` (`mac` = any hunt id, `ch`),
  `huntssid` (`ssid` = a network name, 1..32 bytes of UTF-8, no control characters, not `"0"` - anything else is
  400; null/empty stops; sent unstripped, since an SSID may end in a space; C7, §23),
  `deauth` (`mac` = the AP's BSSID, empty stops; broadcast to every client of that AP), `dca` (`client_mac`, `ap_bssid`; one station), and the
  on/off switches `sdcap`, `events`, `alerts`, `mirror`, `addr1` (truthy `value`), plus `ledtest` (`value` surv|new|join), `page` (`value` next|prev), `blescan`
  (`value` passive|active|auto), `sdinfo`, `sdls`, `sdread` / `sdrm` (`path`, a card-root name checked by
  `card_file_name()`), `info`, and the host-only `explain` (`value` full|current|clear: leave spec mode, re-decode
  Wi‑Fi/BLE/15.4 — all of it, or only the legs covering the frequencies flagged unexplained right now — so the
  unexplained-energy comparison (§18) has fresh known devices, then return to spec; `clear` forgets sticky
  explanations; 409 if one is already running). `patrol` (C5, §22): `value` true|1|"on" starts the default legs,
  false|0|"off"|absent stops, or `legs` = `[[mode, sec], ...]` (or `{"mode","sec"}` objects, or the device's
  `"mode:sec,..."` string) starts custom ones - checked by `clean_patrol_legs()` (2-6 legs, mode
  5g|2.4g|both|ble|154|spec, whole seconds 5-600), 400 otherwise; 409 `patrol: stop <capture|hunt|deauth> first`
  when the host knows one is running. While a patrol runs, starting `capture`, `sdcap`, `hunt`, `deauth`, `dca` or
  `explain` full|current is 409 `<cmd>: stop patrol first` (the stop forms pass; `band` passes and ends the patrol
  on the device). `/api/state` carries `patrol`: `null` (firmware without it), `{"on": false}`, or `{"on": true,
  "leg", "left", "left_ms", "cyc", "legs", "band"}` - `left_ms` counts `left` down from the last status line,
  floored at 0. Read `do_POST` in `bandwatch_host.py` for the exact field names.
  Errors are a non-2xx status with `{"ok": false, "error": "..."}` (400 bad argument or body, 409 refused in the
  current state, 413 body too large, 503 `not connected` when no device is attached — no empty pcap is opened then).

`/api/cmd` has **no authentication**, and one of its commands starts a deauth attack, so the server binds to
`127.0.0.1` by default and, since v1.19.3, defends that against other web pages the browser has open: a POST must
be `Content-Type: application/json` (a cross-site form or a "simple" fetch cannot send that without a preflight);
a POST with a foreign `Origin` is refused; every request's `Host` header must name the bind address, `localhost`,
`127.0.0.1` or `[::1]` (blocks DNS rebinding); and responses carry `X-Frame-Options: DENY`,
`X-Content-Type-Options: nosniff` and a CSP `frame-ancestors 'none'` (no clickjacking). None of this is
authentication: `--bind 0.0.0.0` (the host prints a warning for any non-loopback bind) hands deauth to anyone who
can reach the port. Values that reach the
serial line have embedded CR/LF stripped in `Bandwatch.send()` so a crafted field cannot append a second
command to the line. The pcap writer is touched from both the reader thread and the HTTP thread, so
`handle_frame()` takes a local reference and tolerates the file being closed underneath it.

Vendor names: IEEE OUI CSV cached in `~/.cache/bandwatch/oui.csv`
(downloaded once in the background) with a built-in fallback; Bluetooth company ids, Apple continuity types,
GAP appearance categories and common service UUIDs are small tables at the top of the file.

One dashboard: `dashboard2.html`, served at `/` (and `/v2`, `/v2.html`, `/index.html` for old links). The classic
tabbed `dashboard.html` was removed in v1.19; git tag `v1.18.2` has the last copy. `/classic` and `/classic/`
answer `301` to `/`, and `--ui` is still parsed but ignored (with a note on stdout) so old scripts keep running;
`host/run-v2.sh` went with it. `make_handler(bw, page_path)` takes the one page.
`dashboard2.html`: no framework, polls `/api/state` once a second; controls in a sticky left rail (band + patrol (C5), park, capture, hunt/deauth action
bar, LCD mirror with page buttons, SD card list with pull-to-Mac), one scrolling main column, and only the active
radio's device table open (the other two fold into "last seen" caches). Tables are
rendered keyed by device id so rows keep identity; while the mouse is over a table (or the page is paused with
the button / space bar) rows neither move nor re-render, so buttons stay put. Colours follow a light/dark token
set; charts are inline SVG.

**CSV export (C11, v1.20).** Each device card's filter row (Wi-Fi, BLE, Zigbee) and the "Networks being sought" card
have a small *CSV* button. Pure dashboard - no host or protocol change: every `render{Wifi,Ble,Zig,Probes}()` stores the
rows it just rendered (after filter, checkboxes and sort) with its state snapshot and `hostNow()` in `shown[kind]`,
and the button turns that into a Blob download `bandwatch-<wifi|ble|zigbee|probes>-<YYYYMMDD-HHMMSS>.csv` (local
time). While the page is paused it therefore exports the paused view. The builder is the pure block between the
`// ---- C11: CSV export` markers (`csvField`, `rssiStats`, `CSV_COLS`, `csvBuild`), so it can be sliced out and
tested in Node without a DOM. Columns mirror the tables with raw values (unformatted numbers, full MAC/vendor/SSID),
plus `peak_rssi` (the table's *Max*), `rssi_samples` / `rssi_min` / `rssi_avg` / `rssi_max` over the row's `hist`
(blank with no samples; a `dest_only` row's null RSSI stays blank, as does an unknown channel), `age_s`, and
`last_seen` (ISO-8601 UTC, `hostNow()` minus age at render). Output is RFC 4180 (fields with `,` `"` CR or LF are
quoted, inner quotes doubled, CRLF rows) with a UTF-8 BOM for Excel. SSIDs and names are attacker-controlled, so
any *string* field starting with `=` `+` `-` `@` TAB or CR gets a leading `'` (OWASP CSV injection); numbers are
never prefixed, so RSSI stays numeric. A column added to a table should be added to `CSV_COLS` too.

## 6. Adding things

- **New mode**: extend `BandMode`, `kBandName`, `chanEnabled()`, `setBandMode()` (start/stop functions), the
  header/system/devices page text, `sendHello` (`chs`), and the dashboard mode buttons.
- **New device attribute**: add to the struct in `devices.h`, fill it in the RX callback, append it to the
  row array in `sendDevices()`, parse it in `merge_*()` in the host, render it in the dashboard. Keep the
  protocol table above current.
- **New LCD page**: add to `Page`, write `buildXPage()` / `refreshX()`, wire into `showPage()` / `refreshUi()`
  / `pageAvailable()`.
- **Something new on the SD card**: mount with `sdMount()` and release with `sdUnmount()` in the same loop pass
  unless you own the card for longer (FATFS is ~30 KB, §12); never touch it from a radio context; respect
  `sd.capEnabled` / `sd.readActive` (the card belongs to a capture / an `sdread`); treat a failed open or write
  as "card gone", and change presence only through `sdSetPresent()`. `events.cpp` (§20) is the worked example.

## 7. Quirks and gotchas (read before debugging)

1. **Serial DTR/RTS = BOOT/EN.** Any host program that toggles those lines resets the chip. pyserial: set
   `dtr=True, rts=True` before `open()` (the macOS default state), never `False`. `arduino-cli monitor`: add
   `-c baudrate=115200,dtr=on,rts=on`.
2. **Download-mode latch.** USB-Serial-JTAG can only perform a core reset, which doesn't re-sample GPIO28. If the
   strap was low (BOOT held at power-on), every USB reset lands in "waiting for download". Fix: physical RESET,
   or `esptool --chip esp32c5 --port … --before no-reset --after watchdog-reset run` (build.sh does this when it
   sees the ROM prompt after flashing).
3. **esptool connection attempts reboot the board** repeatedly. Only run esptool to flash.
4. **The host tool holds the port.** Stop it before `./build.sh --upload`.
5. **Crash text is lost** because the USB device re-enumerates on reboot; keep a reader attached, and use the
   `rst` field of `hello` (`panic`, `task_wdt`, `usb`, `poweron`, …) to attribute reboots. Decode panic addresses
   with `riscv32-esp-elf-addr2line -pfiaC -e build/bandwatch.ino.elf 0x4…` (toolchain under `~/.espressif/tools`
   or the Arduino core's `riscv32-esp-elf-gcc`).
6. **OOM signatures:** `abort() was called … lock_init_generic` (newlib couldn't create a mutex) and a store
   fault inside `lv_obj_class_create_obj` (LVGL `malloc` returned NULL). Cut static RAM or widget count.
7. **LVGL 9 `lv_color_t` is 3 bytes**; draw buffers must be `uint16_t` for this RGB565 panel.
8. **Prototype generation cache**: arduino-cli only re-runs ctags when the `.ino` changed. `rm -rf build` after
   changing `tools/ctags/ctags`.
9. **Randomized MACs**: most phones use locally-administered addresses (bit 1 of the first byte set) and BLE
   random addresses rotate; vendor lookup deliberately returns "(randomized MAC)" / nothing for those.
10. **802.15.4 → Wi‑Fi hand-over**: calling `esp_ieee802154_disable()` straight from receive left Wi‑Fi deaf
    afterwards. `stop154()` now does `set_rx_when_idle(false)` → `esp_ieee802154_sleep()` → `disable()`, after
    which Wi‑Fi receives normally. Re-test `154 → 5g` / `154 → 2.4g` (frames > 0) after touching `stop154()` or
    `startWifi()`.

## 8. Testing checklist (serial only, no LCD needed)

1. Flash, wait 3 s, send `info`: `rst` should be `usb`/`poweron`, `promisc` `ESP_OK`, `heap` > 80 000.
2. In `5g`: `d` lines every 220 ms with non-zero `f` on busy channels; `s` every ~5.5 s; `w` lines with APs
   carrying `ssid`, `sec`, `phy`.
3. `band 2.4g`, `band both`, `band ble` (`b` lines, `ble` heartbeat, heap ≥ 30 000), `band 154` (`d` lines on
   11–26, `z` lines if any Zigbee/Thread traffic), then back to `5g` and confirm `f` > 0 again.
4. `park 36`, `cap 1` for 5 s: `P` lines decode as base64, none truncated (`len(base64) == min(len, snap)`),
   `drop` small; `cap 0`.
5. `hunt <mac of a strong AP> <ch>`: `d` lines carry `h: [rssi, age_ms, hits]` with hits rising; `hunt 0`.
6. BOOT tap cycles pages, hold cycles modes; LED changes with score / hunt distance.
7. `cap 1`, then hold BOOT to change band: the device must report `cap: 0` in the next `hello` **and** free the
   capture ring — compare `heap` before `cap 1` and after the mode change; they should match within a few
   hundred bytes. The host closes its pcap on the same `hello`.
8. `deauth <bssid of your own AP>`: `da` rises in the `d` lines, `df` stays at 0. Leave it running past
   `kDeauthMaxMs` (5 min) and confirm it stops itself with a `deauth auto-stopped` log line and `deauth: null`.

## 9. The driver-internal deauth path (fragile — read before touching)

`sendInternalKick()` does not build a frame itself. It calls undocumented symbols inside the prebuilt
`libnet80211.a` (`ieee80211_alloc_deauth`, `ieee80211_send_setup`, `ieee80211_set_tx_desc`, `ic_tx_pkt`) and
patches the descriptor they hand back, because the public `esp_wifi_80211_tx()` sanity check rejects
real deauth frame-control bytes. That means **hard-coded byte offsets into driver-private structs**:

| Offset | Meaning |
| --- | --- |
| `g_ic+16` / `g_ic+20` | STA / AP hmac pointer, passed as arg0 (the driver panics if it is 0) |
| `g_ic+436` / `g_ic+440` | head / tail of the deferred-TX queue used when off the home channel |
| `hmac+312` | hstate: selects which address slot becomes DA / SA / BSSID inside `send_setup` |
| `desc+4` | ebuf pointer P; `*P` holds the header bits, `*(P+4)` the frame bytes |
| `desc+20`, `desc+40`, `desc+52`, `desc+56` | length, address-shift flag, queue link, state block |

These were derived against **core 3.3.11 / ESP-IDF 5.5.5** and nothing checks them at runtime, so a core
whose structs moved will silently corrupt memory instead of failing. A `#warning` in `deauth_diag.cpp` fires if
the core is not 3.3.x — treat it as "re-verify every offset in the table above", not as noise. `setup.sh`
pins `esp32:esp32` 3.3.11 (`ESP32_CORE_VERSION`) for exactly this reason; do not override it casually.

Guards that are in place: a null `desc` and a null ebuf pointer `P` both count a TX failure and return rather
than dereferencing; `serviceDeauth()` stops the attack after `kDeauthMaxMs` so a dead host or unplugged USB
cable cannot leave it transmitting. Failures are visible as `df` in `d` lines and the 4th element of `deauth` in
`hello` (and prove nothing about the air - §11).

**TX power units.** `esp_wifi_set_max_tx_power()` takes **0.25 dBm** units (valid range 8-84, i.e. 2-21 dBm), so
`startWifi()`'s 82 is 20.5 dBm, not the "8.2 dBm" earlier comments and this section claimed (corrected v1.19.3).
Since v1.19.3 the attack sets 84 (21 dBm, the API maximum; it used to pass an out-of-range 160) and `stopDeauth()`
restores 82. The `wifi_country_t` that `startWifi()` sets carries `max_tx_power = 20`, which may cap both at 20 dBm -
unmeasured.

## 10. Concurrency notes (what may touch what)

Only three contexts exist. Everything in `Bandwatch_Loop()` **and** the LVGL timer callback (`uiTimerCb` →
`hopIfNeeded` / `serviceDeauth` / `refreshUi`) runs in the Arduino loop task, because `Timer_Loop()` calls
`lv_timer_handler()` from `loop()`. The other two are the Wi‑Fi task (`promiscuousCb`) and a true ISR
(`esp_ieee802154_receive_done`).

- Device tables and hunt counters are shared with both radio contexts and are only touched under `g_devMux`
  (`portENTER_CRITICAL_ISR` in the radio paths). `g_accum` likewise under `g_accumMux`.
- `g_devRefs` (the compact `DevRef` listing buffer, v1.11) and `devRows` are loop-task only — `sendDevices()` and `refreshDevices()`
  cannot interleave, since both run from `loop()`.
- The capture ring is single-producer (radio) / single-consumer (loop). Freeing it from the loop task
  (`releaseCapture()`) is safe **only** because this is a single-core part: the radio callbacks re-read
  `captureEnabled` and `capRing` on entry and run to completion, so they can never be suspended holding a
  pointer that the loop task then frees. Do not move that free anywhere else, and do not cache `capRing` in a
  local inside the producers.

  The two facts that argument rests on, written down because it reads like a bug if you do not have them
  (it was re-reported as a use-after-free in 1.5.4 and re-checked against the pinned toolchain):
  `CONFIG_FREERTOS_UNICORE=y` and `CONFIG_SOC_CPU_CORES_NUM=1` in
  `tools/esp32c5-libs/3.3.11/sdkconfig`, so there is no second core to run a producer in parallel; **and**
  every producer outranks the consumer — Wi-Fi task priority 23, NimBLE host task 21,
  `esp_ieee802154_receive_done` a true ISR, against the Arduino loop task at 1. A higher-priority producer
  preempts the loop task, never the reverse, so the loop task cannot be scheduled while one is mid-`memcpy`.
  If either fact ever changes — a dual-core target, or the free moved to a task that can preempt — the ring
  needs real synchronisation.
- The event-log queue (`g_evtQ`, 16 × `EvtPending`, 1.18) is filled by `eventFlag()` from the Wi‑Fi and NimBLE
  paths while they already hold `g_devMux` (called when a device slot is created) and drained by
  `serviceEvents()` on the loop task under the same lock. Everything else in the event log — classification,
  baseline, CSV buffer, every SD access — is loop-task only. A full queue drops the event and counts it.
- Strings captured off the air (SSID, BLE name, country code) are stripped of control characters at ingest
  (`sanitizeText`), and JSON-escaped on output (`printJsonStr`). Since v1.19.3 every serial line - device rows,
  `hello`, the spec bin, and every `ack`/`err`/`sdls`/`sdread`/`sdrm` reply - checks `serialRoom()` against a
  budget built from the strings' real *escaped* lengths (or a derived true worst case), not an estimate, so a
  quote- or backslash-heavy SSID cannot outgrow its budget and truncate the line mid-JSON; `sdread_done` waits for
  room rather than being dropped. A full Wi‑Fi chunk (24 rows) or BLE table still needs several KB of the 8 KB TX
  buffer, so new per-row fields are not free.

## 11. Deauth: why it did not work, and what fixed it (resolved 1.6)

> A short post-mortem on why this took seven releases to find — and what to do differently next
> time — is in [`WHY-DEAUTH-WAS-HARD.md`](WHY-DEAUTH-WAS-HARD.md).

Settled with an external witness - a second ESP32 (S3) in monitor mode, `tools/witness/`. Every claim below
is an observation from that witness, not an inference from the device's own counters.

**The attack now works.** With the image patched (below), a witness on the target channel hears real
deauthentication frames: correct `0xC0 0x00` subtype, spoofed SA/BSSID, broadcast DA, reason 7, sequence
numbers incrementing, at -38 dBm. 300 frames accepted by the driver, 289 heard on air.

### What was actually wrong

Two separate faults, on two separate paths, each masking the other:

| path | device reports | on air | fault |
| --- | --- | --- | --- |
| raw `esp_wifi_80211_tx()` | `ESP_ERR_INVALID_ARG` 304/304 | nothing | subtype gate rejects deauth |
| internal slot (`sendInternalKick()`) | no error, `da` climbing | nothing | descriptor never reaches the PHY |

The internal path was the default and was believed to be "the real attack path". It is not: forcing a
**beacon** down that same descriptor path (`kickfc 80`) also radiates nothing, while the identical subtype
through `esp_wifi_80211_tx()` is heard at -45 dBm. So it is not a subtype problem there - **the §9 offsets are
wrong and that path has never transmitted anything, of any kind.** Because it silently "succeeded", the raw
path never ran, and the loud, fixable failure stayed hidden behind the silent, unfixable one.

### The fix

1. **Default to the raw path** (`useInternalKick = false`). It either works or fails loudly with
   `ESP_ERR_INVALID_ARG`; the internal path fails silently while reporting success. `kickpath 1` selects the
   internal slot again for anyone working on the §9 offsets.
2. **Patch out the subtype gate.** `esp_wifi_80211_tx()` calls `ieee80211_raw_frame_sanity_check()` first and
   bails if it returns nonzero:

   ```
   420ff5c6:  jal  420ff436 <ieee80211_raw_frame_sanity_check>
   420ff5cc:  bnez a0, ...        # nonzero -> return, nothing transmitted
   ```

   `tools/deauth/patch_raw_tx.py` overwrites its prologue with `c.li a0,0 ; c.jr ra` - four bytes.

Espressif document the restriction: *"Currently only support for sending beacon/probe request/probe
response/action and non-QoS data frame"*. Deauthentication is not on that list; beacon is, which is exactly the
split the witness measured before the patch.

**Nothing downstream filters the subtype.** Once the gate returns 0 the frame goes out, so the rest of the TX
chain (`ic_ebuf_alloc` -> rate/schedule setup -> `ieee80211_post_hmac_tx`) is subtype-agnostic. Reimplementing
that chain to avoid the patch is therefore possible but unnecessary.

### Why `-Wl,--wrap` is not an option

The usual escape fails: in core 3.3.11 the check and its only caller are both inside `ieee80211_output.o`, so
the call is bound within the object file and is never an undefined reference for `ld` to redirect. Tested - the
linked image comes out byte-identical and still calls the original. Same layout on esp32, esp32s3, esp32c3,
esp32c5 and esp32c6, so no other chip in the family avoids it. An ESP-IDF rewrite does not help either:
`libnet80211.a` is the same closed blob and the 4277-line sdkconfig has no deauth-related option. Rule 7 is
real but orthogonal to this problem.

### Using it

```
./build.sh --upload                      # patched by default
python3 tools/witness/verify.py          # confirm from the air, never from the counters
```

`build.sh` applies `tools/deauth/patch_raw_tx.py` to the linked image before flashing, then restores the
stock `.bin` in the build directory so it is never left in a patched state. `--no-patch` flashes the stock
image instead, whose deauth fails loudly with `ESP_ERR_INVALID_ARG` and transmits nothing.

**The patch is not in the source tree and cannot be.** It edits `libnet80211.a`'s code inside the linked
output, so every build must be patched again - which is why it is wired into `build.sh` rather than left as
a step to remember. It edits the build output, not the toolchain's copy of the blob: patching that would
disable the check for every ESP32 project on the machine.

**The image must be resealed.** An app image carries a 1-byte XOR checksum over segment data plus an appended
SHA-256. Patching without recomputing both makes the second-stage bootloader refuse the image
(`Checksum failed ... No bootable app partitions`) and the board loops until BOOT is held to force ROM download
mode. `reseal_image()` handles this; its self-test is that resealing an *unpatched* image reproduces it
byte-for-byte.

**Fragility.** The address is resolved from the ELF on every run because it moves whenever the sketch changes -
it shifted from `0x420ff3b0` to `0x420ff436` just from adding a diagnostic command. Never hardcode it. This is
rule 8 fragility squared: pinned to one exact core build, and a core bump changes behaviour silently. Disabling
the check also lets malformed frames through generally, not only deauths - suspect the patch first if the
firmware misbehaves. Re-verify with the witness after any toolchain change.

### Ruled out along the way

| Hypothesis | How it was tested | Result |
| --- | --- | --- |
| PMF / 802.11w protecting the BSS | read the RSN capabilities out of the beacon | PMF is `none` - not it |
| DFS blocking TX on channel 64 | ran the identical attack on non-DFS ch 2 | fails identically - not it |
| Promiscuous mode blocking TX | `esp_wifi_set_promiscuous(false)` then inject | no change - not it |
| The radio cannot transmit at all | `txtest 1`, witnessed | it can - 323 beacons at -45 dBm |
| The deauth *subtype* is what the internal path drops | `kickfc 80`, witnessed | no - a beacon there also radiates nothing |

**Corrected:** the comment justifying the SoftAP PoC claimed raw TX "radiates nothing from an unassociated
STA". False - `txtest 1` transmits from `WIFI_IF_STA`, unassociated, no SoftAP, witnessed at -45 dBm. Also,
macOS redacts SSIDs in `system_profiler SPAirPortDataType`, which is why a Mac was never a usable witness.

**The counters still lie.** `deauthSent` (`da`) is incremented at the end of `sendInternalKick()`
unconditionally and `ic_tx_pkt()` returns `void`; on the raw path an `ESP_OK` only means queued. `da` reached
328 in a window where the witness heard zero. Confirm on air, always.

### Still open

Nothing for the basic attack: real stations disconnect - confirmed by the owner on their own network (2026-10-08, v1.19.x): stations drop,
with devices they own on an AP they own. Not measured: which clients and how fast; unprotected-frame handling varies
by supplicant, and PMF-enabled networks ignore these frames by design.


## 12. microSD pcap recording (1.3)

The device writes pcap files itself, so a capture is not limited by USB CDC throughput and can outlive the
host connection. `sdcap 1` starts, `sdcap 0` stops.

**Wiring and bus sharing.** The card is on the *same* SPI bus as the LCD (SCLK 7, MOSI 6, MISO 5), with its
own CS on **GPIO4**. This is safe because `Display_ST7789.cpp` wraps every LCD transfer in
`SPI.beginTransaction()/endTransaction()` with its own `SPISettings`, the SD library does the same at
`kSdSpiHz` (20 MHz vs the LCD's 40 MHz), and both are driven from the loop task — there is no second task
competing for the bus. Do not move SD access into a radio callback or a separate task without adding real
bus arbitration.

**File format.** `sdWriteFrame()` is a direct port of `PcapWriter` in `host/bandwatch_host.py`: identical
global header, identical 24-byte radiotap (TSFT | Flags | Channel | dBm, link type 127) and 28-byte
802.15.4-TAP (FCS-type / RSS / channel TLVs, link type 283). Files from the device and from the host are
interchangeable — verified by pulling a capture back with `sdread` and opening it with Wireshark's
`capinfos` (2051/2051 packets, no malformed records).

**Clock.** There is no RTC. The host sends `time <epoch>` on connect; that drives both the pcap record
timestamps and the filename. Device-written names are **UTC** (`/bandwatch-wifi-YYYYmmdd-HHMMSS.pcap`) while
the host names its own files in local time — do not "fix" one to match the other without deciding which is
authoritative. With no clock ever set, files fall back to `/bandwatch-wifi-NNNN.pcap` and timestamps start
at the epoch.

**Memory — the part that constrains the design.** Mounting FATFS costs about 30 KB of heap, and BLE mode
needs headroom. So the card is mounted **only while in use**:
probed once at boot (`sdProbeAtBoot()` records `sdCardPresent`/`sdCardMb`, then unmounts), mounted again by
`sdcap`/`sdinfo`/`sdls`/`sdread`, and released by `sdUnmount()` when done. Measured on hardware in v1.3 (before
the v1.11 reclaim and the v1.15 custom-drawn pages; §16 has current figures):

| state | free heap |
| --- | --- |
| idle, card present but unmounted | 83.4 kB |
| recording to SD | 31.9 kB |
| after stopping | 83.4 kB |
| BLE mode (floor is 28 kB) | 57.8 kB |

`sdcap` opens the file **before** sizing the capture ring, so `kCapHeapReserve` is reserved against
post-mount heap. The ring therefore gets fewer slots while recording to SD (§16: 11 slots after `cap 1` then
`sdcap 1`, 9 with `sdcap` first, measured in v1.15.4; v1.15.5's lighter Overview page let USB + SD keep all 20) — expect a
higher `drop` count on a busy channel than with USB capture alone. An earlier ordering left only 12.5 kB
free, which is inside the range where LVGL page rebuilds fail (`CLAUDE.md` rule 4); keep the current order.

**Sinks.** USB streaming and SD recording are independent consumers driven from one ring and one tail in
`drainCapture()`. If USB is enabled but its TX buffer is full *and* SD is recording, the USB copy is skipped
and counted in `drop` rather than stalling the ring — the card must not lose frames because nobody is
reading the serial port. With USB alone the old stall-and-retry behaviour is kept.

**Latency.** SD writes are buffered to `kSdBufSize` (4096 B, matching `CONFIG_FATFS_SECTOR_4096`) and fsynced
every `kSdFlushMs` (5 s), so a power cut costs at most a few seconds. Because card GC can stall a write for
tens of milliseconds and `hopIfNeeded()` shares this task, `drainCapture()` gives SD at most `kSdBudgetUs`
(8 ms) per loop iteration. Park on one channel for long captures; while hopping, heavy SD load will skew
dwell timing and therefore the busy score.

### Deleting card files (`sdrm`, C4 follow-up)

`sdrm <name>` (`sdRemoveFile()` in `sd_sink.cpp`, loop task like every card user) deletes one file in the card
root. The rules, in the order they are checked:

- **Plain names only.** One optional leading `/`, then 1-39 chars of `[A-Za-z0-9._-]` and no `..`, so no nested
  path and no traversal. The 39 is the same limit as the host's `sdread` guard: the device's command line holds
  47 chars and `sdrm /` takes 6, so a longer name would arrive truncated and delete a *different* file. The
  charset also keeps the name safe to echo in the JSON ack. Every name the firmware writes fits.
- **Not while an `sdread` streams** (any file: the pull holds the card and one open file handle).
- **Not the file `sdcap` is recording** (`sd.path`). Other files may go while a capture runs: the capture's mount
  is reused and `sdUnmount()` leaves it in place.
- **Not while the event log is mid-flush** (`eventsFlushing()`: `flush()` has `/events.csv` / `/seen.csv` open
  between `attachCard()` and its unmount, and a `/seen.csv` register pass keeps the file open across loops, §20). Commands and flushes both run on the loop task, so today a command
  always lands between flushes; the check keeps that true if flushing ever becomes incremental.
- **Mount on demand, unmount after** (`sdMount()` / `sdUnmount()`); no card → `sdrm: no card`.
- **Deleting `/seen.csv` while the event log is armed resets novelty** (`eventsFileRemoved()` in `events.cpp`): the
  baseline is reloaded from the card - now empty - instead of keeping the RAM set, and the MACs still waiting to be
  appended are dropped with the file they belonged to (their `new` rows are already in `/events.csv`). From then on
  every globally unique device counts as new again and `/seen.csv` starts over. Deleting `/events.csv` needs
  nothing special: the next flush recreates it with its header, and rows still buffered land there.

The host (`POST /api/cmd {"cmd":"sdrm","path":"/name"}`) applies the same guard as `sdread` (`card_file_name()`:
the device's pcap names or `CARD_TEXT_FILES`), drops the file from its cached listing when the ack says `ok` and
re-asks `sdls`. **Copies already pulled into the captures dir are never touched** - deleting on the card is not
deleting the local file, and the dashboard says so under the list. Dashboard v2 lists every card file (newest
four pcaps plus `events.csv` / `seen.csv` by default, "show all" for the rest), each with Download (pull ->
progress -> the browser saves it) and a two-click Delete (`Delete` -> `Really delete?` for 4 s; no
`window.confirm()`, which blocks automation), disabled for the file being recorded and during a pull.

### Removal, insertion and a missing card (1.18)

Until 1.18 `cardPresent` was set once by the boot probe and nothing noticed a card being pulled: a capture's
writes failed and the drain loop closed it silently, `sdread` sent `sdread_done` for a short file (which the
host saved as complete), and `sdinfo` plus the failure paths of `sdOpenCapture()` / `sdls` left FATFS (~30 KB)
mounted. Now:

- **One place presence changes:** `sdSetPresent(present, why)` in `sd_sink.cpp`. `sdMount()` reports every
  attempt through it, and so does every failure path. The boot probe's reading only sets the state; after that a
  real transition shows the LCD face (below), emits `{"t":"log","msg":"sd card removed|inserted[: why]"}` (which
  the host uses for `sd.mounted`) and, on insertion, calls `eventsNudge()` so the event log (§20) attaches now
  instead of on its next 30 s retry.
- **`sdMount()` calls `SD.end()` before `SD.begin()`**, so a re-inserted (or swapped) card gets a fresh init
  rather than the stale handle the vanished card left behind.
- **Capture write failure** sets `sd.ioFailed`, and `sdServiceFlush()` is the one path that handles it: the
  `sdcap: write failed after N frames (card removed?) - recording stopped` error, `sdCloseCapture()`, `SD.end()`,
  presence → removed (`recording stopped`), then `syncCapActive()` so the ring goes back to the heap unless the
  USB sink still uses it.
- **`sdread` short read** sends `sdread: read failed at X of Y bytes (card removed?)` instead of `sdread_done`
  and marks the card removed (`file pull stopped`); the host discards the half-built buffer.
- **`sdinfo`** unmounts after replying (`sdUnmount()` keeps the mount when a capture or `sdread` owns it).

**Presence probe without a card-detect pin.** The board has none, so `sdServicePresence()` (loop, every 2 s)
sends one SPI CMD0 at 400 kHz (`sdProbeR1()`: ≥ 74 clocks with CS high, CMD0 with CRC 0x95, read R1). A card
answers `0x01` (idle); an empty slot leaves MISO high, `0xFF` (verified, below). Under 1 ms, no FATFS, no heap, inside
`beginTransaction/endTransaction` like every other user of the bus. It runs **only while the card is idle and
unmounted** — never while FATFS is mounted, a capture records or an `sdread` runs, because CMD0 resets a card
mid-session; mounted users learn of a removal from their own I/O errors. Two agreeing consecutive readings are
needed to change state (debounces a card mid-insert). `sdprobe` returns the raw R1 for testing.

**LCD faces.** On a presence transition (not at boot) `showSdFace()` (`lcd_ui.cpp`) creates one custom-drawn,
full-screen object on `lv_layer_top()`, so it sits above any page, mode card or photo and survives a page rebuild
underneath; `serviceSdFace()` deletes it after `kSdFaceMs` (3 s). Card out: a pale-blue face with a frown,
"SD card out" and the reason in grey (e.g. "recording stopped"). Card in: a yellow smiling face, "SD card in".
A second change while one is up repaints it with the new mood. `sdface 0|1` shows either for testing.

**Measured on hardware (1.18 session).** Card pulled during `sdcap` with the event log armed:
`sdcap: write failed after 619 frames (card removed?) - recording stopped`, the capture closed cleanly (166 kB
on the card), the event log went to `card: 0` and kept buffering (21 rows), and 2 novelty checks were skipped
(`wait`). Re-inserted: re-attached, baseline reloaded, the 21 rows flushed (`written: 52`), nothing lost.

**Probe verified on hardware.** With the slot empty CMD0 reads R1 = `0xFF` (255) because MISO rests high; with a
card in it reads `0x01`. The two are cleanly distinguishable on this board. Detection timing, measured: a pulled
card gave its first absent reading at 186.2 s uptime and `sd card removed` was logged at 187.3 s (the second
agreeing reading); an inserted card logged `sd card inserted` at 204.2 s. Worst case is about 4 s (2 s probe
interval × 2 readings). No open items remain for the probe.

**Hot-pulling can reset the board (intermittent, hardware).** One of three hot-pulls coincided with a reboot whose
reason was `rst: usb` — a USB-peripheral reset, not a panic. The other two (one during `sdcap`, one idle with the
probe running) caused no reboot and no USB disconnect. The likely mechanism is a supply dip as the card's contacts
break, briefly dropping the USB link; the host's reconnect then changes DTR/RTS, which resets the chip (CLAUDE.md
rule 1). It is not caused by the probe. Pull the card gently and do not touch the USB cable while doing it.


## 13. BLE capture (1.4)

BLE records as `LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR` (256). Verified with Wireshark: 2276/2276 packets
dissected, zero malformed, correct `ADV_IND` / `ADV_NONCONN_IND` / `SCAN_RSP` types, addresses and company
IDs resolved.

**Why the Arduino BLEScan wrapper had to go.** `BLEAdvertisedDevice` cannot give packet-accurate data:
`parseAdvertisement()` *merges* every payload from an address into one growing buffer (its own comment:
"handles both ADV and Scan Response packets by merging them"), so `getPayload()` is an accumulation rather
than a packet; with active scanning `onResult()` fires only once the scan response arrives, with the merged
device; and `setAdvType()` is recorded only when the device is first created, so the PDU type goes stale.
There is no `setMaxResults()` in the wrapper's public API to force per-report objects. Bandwatch therefore
drives `ble_gap_disc()` directly (`host/ble_gap.h` ships with the core) with its own GAP event handler, and
parses AD structures itself (`parseAdStructures`). Approach borrowed from
[shermanatoor/ouispy-blesniff](https://github.com/shermanatoor/ouispy-blesniff).

Side benefits: no library result cache, so the heap-floor dance `serviceBle()` used to do is gone and BLE
mode idled at ~95 kB free instead of ~58 kB (measured in v1.4; ~88 kB in the 2026-10 audit at v1.15.2, after
static RAM grew - §16), and the device table sees slightly more devices.

**Record layout** — 10-byte pseudo-header written by the pcap writers from `f.channel`/`f.rssi`, then the
reconstructed LL packet built by `buildBleLlFrame()`:

| field | value |
| --- | --- |
| phdr channel | 39 — the HCI report does not say which of 37/38/39 it arrived on |
| phdr flags | `0x0013` dewhitened + signal-power-valid + ref-AA-valid; `0x0011` when RSSI is 127 (the HCI "not available" sentinel) |
| CRC-checked / CRC-valid flags | **deliberately clear** — we synthesize a zero CRC, and claiming "checked" would make Wireshark mark every frame CRC-bad |
| access address | `0x8E89BED6` (advertising channel) |
| LL header | PDU type in bits 0-3, TxAdd set when AdvA is random. RxAdd left clear: the report never carries TargetA's type, and guessing writes a fabricated fact into the capture |
| length field | 6 bits covering AdvA + AdvData, so AdvData is **truncated** at 57 — masking an over-long length wraps it to a bogus value and Wireshark mis-dissects the record |
| CRC | three zero bytes |

HCI event types are **not** the same numbers as LL PDU types (`ADV_SCAN_IND` is HCI 2 but LL 0x6,
`ADV_NONCONN_IND` is HCI 3 but LL 0x2) — `llPduTypeFromHci()` maps them.

**What this is not.** Advertising packets only, reconstructed from HCI reports. No connection or data-channel
traffic, no real channel number, no real CRC, legacy advertising only (extended advertising is compiled out
of the core). It is not a BLE sniffer in the Ubertooth/nRF sense and must not be described as one.

**Address byte order.** `ble_addr_t.val` is little-endian, the order the address goes on air. The pcap
builder writes those bytes as-is, which is correct for the wire format. Everything that *displays* or keys
on a MAC - device table, LCD, dashboard, OUI lookup, the locally-administered (random) bit, hunt matching -
expects conventional MSB-first order, so `bleGapEvent()` reverses into a separate buffer for
`trackBleDevice()`. Getting this wrong (as 1.4 did) is quietly nasty: names land under mirrored keys, OUI
lookup reads the device-id end of the address, and the random-address bit is read from the wrong byte.

**Scan policy** (`blescan passive|active|auto`, default **auto**). Passive only listens, so we never
transmit, but most device names live in scan responses that only arrive if something sends `SCAN_REQ` —
measured: 2 named devices passive vs. 4 active in the same spot. Active scanning solicits them, which both
fills in names and puts genuine `SCAN_RSP` packets in the capture as their own records; the cost is that the
board is transmitting. Note the pre-1.4 code used active scanning unconditionally, so the README's "never
transmits" claim was already inaccurate in BLE mode.

**auto** is the default and gets most of the benefit for a fraction of the airtime: stay passive, and open a
`kBleActiveWindowMs` (4 s) active window only when a *new* address appears that is **scannable** and has no
name yet. Non-connectable beacons never answer a `SCAN_REQ`, so asking for one would transmit for nothing.
Two guards matter:

- The switch is requested by a flag from the GAP callback and applied in `serviceBle()` on the loop task -
  restarting discovery from inside the callback would re-enter the NimBLE host.
- `kBleSwitchMinMs` (2 s) rate-limits flips, because each one cancels and restarts discovery and so costs a
  short gap in reception. In a crowded place with rotating random addresses, new MACs arrive constantly and
  an unthrottled policy would sit permanently active.

**The mode is frozen while a capture runs** (`if (capActive) return;`), so a single pcap is never half
passive and half active. Whatever is running when recording starts stays for the whole file.


## 14. Capture telemetry gotchas (1.4.2)

Two bugs made SD recording *look* broken while the device was in fact writing the file correctly. Both were
in the reporting path, and both are easy to reintroduce:

1. **BLE mode has no `{"t":"d"}` lines.** The SD counters (`sdc`/`sdf`/`sdb`) were only added to `sendDwell()`,
   so in BLE mode the dashboard never saw progress: the frame count sat still and the Record button never
   flipped, even though the card was filling. `sendBleStatus()` now carries the same fields. **Rule: any
   live counter the dashboard needs must go on both the dwell line and the BLE heartbeat.**
2. **Ack key mismatch.** The device's ack is `{"cmd":"sdcap","sdcap":1,...}` but the host was copying a key
   named `cap`, so `sd.cap` was never updated from the ack. In Wi-Fi the next dwell line masked the bug;
   in BLE nothing ever corrected it. The host now maps `sdcap` -> `cap` explicitly.

Symptom to remember: if a capture writes a file on the card but the dashboard shows nothing happening,
suspect the telemetry path, not the writer. `sdinfo` over serial reports the device's own truth.

**LCD record light.** `recording()` is true when either sink is active; `applyRecColor()` paints the page
header red and `recTag()` appends ` USB` / ` SD` / ` REC`, on the Overview, Channels, Devices and Hunt
headers. The RGB LED pulses in parallel (`driveLed`), cyan for the host sink, magenta for the card, white
for both - placed after the hunt branch so hunting keeps the LED, but before the busy-score colours.


## 15. Receiver-side sightings and surveillance flagging (1.5)

### addr1 (tier 1) tracking

`promiscuousCb()` used to look only at `addr2`, the transmitter. Devices that sleep through most of a dwell
window - Flock Safety cameras being the documented case - are then invisible, because they rarely transmit
while we are listening on their channel. They do, however, appear as `addr1`, the **destination** of frames
sent by nearby APs. Tracking addr1 surfaces them. The technique is OrdoOuroboros / @NitekryDPaul's, via
[flock-you](https://github.com/colonelpanichacks/flock-you).

It is deliberately weaker evidence, and the upstream project is explicit that addr1/addr3 matching
**does misfire** - an AP answering a wildcard probe can name an unrelated address. So it is tiered:

| tier | meaning | in the protocol |
| --- | --- | --- |
| 2 | we heard the device transmit (`addr2`) | `flags` bit2 clear |
| 1 | only ever seen as a destination (`addr1`) | `flags` bit2 set, `dest_only` in the host |

Rules that keep tier 1 from polluting the table, all in `trackWifiDevice(..., destOnly)`:

- a tier-1 sighting **never evicts** a slot holding a device we have actually heard;
- it never writes `rssi`, `maxRssi`, `ch` or beacon IEs — that signal belongs to whoever *sent* the frame,
  not to the device being addressed. Only `frames` and `lastMs` advance;
  a fresh tier-1 slot does not seed `maxRssi` from the sender either (fixed in v1.19.3), and the host reports
  `rssi`/`max` as `null` for `dest_only` rows, which the dashboard shows as "–";
- the first `addr2` sighting clears bit2 and promotes the entry to tier 2, permanently;
- broadcast/multicast destinations and frames where `addr1 == addr2` are skipped.

Toggle with `addr1 0|1` (default on). Measured in a normal flat: 38 devices, 35 tier 2, 3 tier 1 — three
devices that a transmitter-only sniff would not have listed at all.

### Surveillance OUI flagging

`kSurvOuis` in the firmware holds **64 prefixes across 7 categories** (Flock Safety, Ring, Axon, DJI,
Parrot, Skydio, Meta/Ray-Ban), from colonelpanichacks/ouispy-detector plus @NitekryDPaul's Flock research.
The firmware does the match so the **LCD** can flag too, and reports a category id; the host names it
(`SURV_CAT` / `SURV_KIND`) and derives a tier. Wi-Fi and BLE tables both carry it.

- LCD Devices page: `!` and orange for a surveillance hit, `~` and grey for a tier-1 destination-only entry.
- Dashboard: a red badge in the device tables, outlined and suffixed `?` at tier 1, plus a
  **surveillance only** filter on the Wi-Fi tab.

**An OUI match is evidence, not proof.** Prefixes get reassigned and vendors buy blocks from each other;
upstream had to withdraw two Flock prefixes as Ubiquiti false positives. Treat a hit as "worth a look",
never as identification, and do not let the UI imply otherwise.

### sdread no longer blocks the loop

`sdReadFile()` used to stream the whole file inside the command handler. That handler runs on the loop task,
which also drives `lv_timer_handler()` -> `hopIfNeeded()`, so pulling a large capture stalled channel
hopping for the duration and skewed every dwell in that window. It is now incremental: `sdReadFile()` opens
the file and `serviceSdRead()` emits what fits in the TX buffer each loop, bounded by `kSdBudgetUs`.
Measured pulling a 2.2 MB capture while hopping: 368 kB/s, 25 dwells delivered during the transfer, median
dwell interval 233 ms against a 220 ms nominal.


## 16. Memory: where the ~80 kB rule comes from, and what actually binds

`CLAUDE.md` rule 4 tells you to keep static RAM "well under ~80 KB". It is worth knowing that this is an
**empirical heuristic, not a derived limit** — and what the real constraint underneath it is.

### Origin

The rule entered the repo in commit `265d955` (1.2), after real out-of-memory crashes with two signatures:

- `abort() was called … lock_init_generic` — newlib could not allocate a mutex;
- a store fault inside `lv_obj_class_create_obj` — LVGL's `lv_malloc` returned NULL.

Static RAM was cut, the crashes stopped, and "stay under ~80 kB" became the rule. Nobody has re-derived the
number since. Treat it as a budget line someone drew after being burned, not as a hardware boundary.

### The mechanism is real even though the number is arbitrary

The C5 has no PSRAM. Static allocations (`.data` + `.bss`) and the runtime heap come out of the **same**
~320 kB DRAM pool, so every static byte costs a heap byte one-for-one. Adding a global is not free just
because the build still links.

### The build output is misleading — do not trust it

arduino-cli reports something like:

```
Global variables use 79576 bytes (24%) of dynamic memory, leaving 248104 bytes for local variables.
```

That "leaving 248104 bytes" is the linker's arithmetic, not reality. Measured free heap at runtime is
**~99-106 kB** in `both` depending on the LCD page (v1.15.5; it was ~83 kB before the v1.11 reclaim), because the Wi-Fi/BLE driver stacks and the FreeRTOS task stacks claim the rest once the radio
comes up. Reading the linker figure literally suggests roughly 3× more room than exists, which is a good way
to talk yourself into a change that then crashes in the field.

### What actually binds: free heap at peak concurrent load

Measured on hardware (read `heap` from `hello`, or the dashboard). Each row names the release it was measured
in; static RAM has grown since some of them (74,032 B at v1.15.4 → 77,392 B at v1.19), so treat older rows as
upper bounds:

| state | free heap |
| --- | --- |
| idle, Wi-Fi `both` (v1.15.5: Overview / System / Devices / Channels) | ~98.9 / 102.3 / 106.4 / 106.4 kB |
| idle, BLE (v1.15.2, 2026-10 audit) | ~88 kB |
| peak: event log armed + USB + SD capture, every LCD page (v1.19) | **25.2 kB** (floor 24 kB) |
| USB + SD capture, 20-slot ring, worst page Overview (v1.15.5) | ~27.8 kB |
| SD capture (v1.3) | 31.9 kB |
| BLE + SD capture (pre-1.11) | 32.3 kB |
| SD capture with the pre-1.3 ring ordering | 12.5 kB — where LVGL page rebuilds start failing |

The worst case is the capture ring (up to 32 kB), FATFS (~30 kB) and an LVGL page rebuild all landing
together. That last row is not hypothetical: 1.3 shipped briefly with the ring sized *before* the FATFS
mount, which left 12.5 kB free and put the device back in exactly the regime the original crashes came from.
`sdcap` now opens the file before sizing the ring so `kCapHeapReserve` is reserved against post-mount heap.

**The LCD page is a big swing on its own.** The 2026-10 audit (`both` band, `tools/probe_pages.py`) measured
System ≈ 101.4 kB free, Devices ≈ 90.3, Overview ≈ 86.8, **Channels ≈ 55.8** - 39 rows × row/2 labels/bar was
~156 LVGL objects. v1.15.4 draws that grid as **one object** (`chanGridDraw()` on `LV_EVENT_DRAW_MAIN_END` in
`lcd_ui.cpp`, painting from a 3-byte `ChanCell` snapshot that `refreshChannels()` fills; only cells that changed
are invalidated, so the LCD and the §19 mirror see small dirty regions, not a 262-row repaint). Measured after:
Channels **≈106.3 kB** (+50 kB), Overview ≈87.6, Devices ≈91.2, System ≈102.2 - Overview is now the heaviest.
Text in the drawn grid is clipped to its column by narrowing `layer->_clip_area` around each `lv_draw_label`,
which is what `LV_LABEL_LONG_CLIP` did for the widgets.

A page switch allocates on the LVGL side, which nothing on the capture side could refuse: a capture sized while
on a light page went under the floor on the next BOOT tap (measured 16.4 kB, Channels → Overview). So `showPage()`
records each page's build cost (largest seen, logged as `page N build cost B` when it grows: Overview ~20 kB,
Devices ~16 kB, System ~5.7 kB, Channels ~1.9 kB), and `lcdPageHeadroomB()` - the step from the current page to
the heaviest page available in this mode - is added to `kMinFreeHeapB` when the ring is sized or re-fitted.
Measured: USB + SD capture started on any page bottoms out at **~30.8 kB** on Overview.

v1.15.5 gave Overview's per-channel strip (`ovStripDraw`, one `OvBar{x,h,col565}` per visible channel, laid out
the way the old `SPACE_BETWEEN` flex row did) and the Devices list (`devListDraw`, painting `devRows[]` with an
FNV fingerprint per row in `devRowHash[]` so only changed rows repaint) the same treatment. Free heap in `both`:
Overview ≈98.9 kB (+11.3), Devices ≈106.4 (+15.2), Channels ≈106.4, System ≈102.3 - a ~7.5 kB spread, with
Overview still the heaviest (its remaining cost is the header, global bar and Top-3 widgets). USB + SD capture
now keeps a full 20-slot ring and bottoms out at ~27.8 kB on Overview. Drawn text that must stay on one line
sets `LV_TEXT_FLAG_EXPAND` - `lv_draw_label` word-wraps inside its box, unlike `LV_LABEL_LONG_CLIP`. v1.16.1 moved
the Spectrum page's 42 bars onto the same `BarStrip` drawing (each bar an exact 1/42 slice of the width, less a
1 px gap) - the old flex row overflowed its panel and clipped the top ~6 MHz. No page builds per-item widgets now.
Montserrat 12/14/20 are built without U+00B7 and the dashes: LCD strings must stay ASCII or they render as boxes.

**The event log (§20, 1.18) is heap while armed, nothing while off:** the baseline set (`kBaseCap` 2,560 ×
4 B = 10,240 B), the CSV row buffer (2,048 B) and the pending `/seen.csv` appends (48 × 12 = 576 B since the v2
register) — about 12.9 KB, malloc'ed by `events 1` (refused with an error if it will not fit) and freed by `events 0`. It mounts
FATFS only for the length of a flush and never alongside a capture (flushes wait while `sdcap` records), so the
FATFS cost does not stack on the SD-capture worst case above — but the 12.6 KB are gone from *every* state while
armed, BLE and capture included, and arming is persisted. Free heap at peak load with the log armed was
measured in v1.19: **25,172 B** (event log + USB + SD capture, every LCD page; §20). Static cost of the 1.18 work (event log + presence probe + faces): 76,816 → **77,272 B** (the
16-entry radio queue is 160 B of it). A `/surveil.csv` on the card adds up to 64 × 4 B = 256 B of heap, loaded
once at boot.

### The floor, as built (see [ROADMAP.md](ROADMAP.md))

The proposal landed in `ensureCapRing()` (`capture.cpp`): right after allocating the ring it checks total free
heap against `kMinFreeHeapB` (24 kB); below that the ring goes back to the heap and capture is refused with a
JSON error — better than OOMing an LVGL page rebuild later. Since 1.15.3 the ring is also *sized* to leave the
floor standing (it shrinks toward `kCapSlotsMin` instead of being refused), and `sdcap` calls `refitCapRing()`:
a ring that `cap 1` made before the FATFS mount was floor-checked against the pre-mount heap, so `cap 1` then
`sdcap 1` used to land at **16.1 kB** free (measured). Refit re-sizes it against the post-mount heap without
touching either sink (20 → 11 slots, 30.5 kB free; the `sdcap`-first order gives 9 slots, 33.8 kB). It guards only the one biggest allocation (plus the page-switch
headroom above), so rule 4 still keeps the rest honest: current static usage is 77,184 B (v1.19.3; 77,392 B at v1.19.1; 77,272 B at v1.18; 74,032 B at v1.15.4), under 3 kB
below the line, and that headroom is the edge of an unverified budget rather than a wall.

### Where the static RAM went (symbol-level, v1.5.5 -> v1.10 -> v1.19.1)

Measured by diffing `.dram0.data`/`.bss` symbols (`nm -S --size-sort`) between builds of each tag, not estimated.
v1.5.5 -> v1.10 grew static RAM from 79,592 to 80,544 B (**+952 B**); about 95% of that is spectrum mode (v1.7/v1.8),
not the v1.10 mode splash:

| Symbol | Delta | What it is |
| --- | --- | --- |
| `specFine[84]` | +504 B | v1.8 fine-spectrum bin array (`SpecBin` x 6 B) |
| `channels[]` | +216 B | v1.7 added `edMin/edMax/edMean/edSamples` to `ChannelState`: +4 B x 54 channels |
| `spBars` (`lcd_ui.cpp`) | +168 B | v1.7 LCD spectrum page bar array |
| splash + button-walk state | ~+30 B | `splashTitle/Sub/Bg/DurMs/StartMs`, `pages[]`, walk vars - all of v1.10 |
| energy accumulator | ~+25 B | `s_edSum` etc., `currentSpecMhz`/`specStepMhz` |
| misc | +-15 B | `kickFc` +1, `softApPoc` -1 (removed SoftAP PoC), 1.6/1.9 bits |

Since then (80,544 -> 77,392 B at v1.19.1): the 4 KB `DevSnap` union became the compact `g_devRefs` listing (+960 B,
~-3.3 kB, net **-3,136 B**), which nearly offsets v1.17's `kWifiDevSlots` 64 -> 96 (+2,048 B) and the `BleDev` repack
56 -> 48 B (-384 B); the C-series features (`probeSeen`, `probeQ`, `evtQ`, prefs) add about +1 kB.

## 17. Station-to-BSS association (1.5.4, ported to the module layout in 1.5.5)

Every station row can now say which BSS it is on. This is what makes the targeted deauth (`dca`) usable from
the dashboard: picking a client to kick requires knowing which AP it belongs to, and before 1.5.4 nothing in
the firmware, the host or the UI tracked that (the dashboard filtered on a `parent` field that was never
produced by anything, so the client picker was permanently empty).

**Where the BSSID comes from.** `promiscuousCb` (`wifi_sniff.cpp`) reads the two DS bits of the frame control field and uses
only the unambiguous single-hop cases (802.11-2020 9.3.2.1, table 9-26):

| ToDS | FromDS | addr1 | addr2 | addr3 | what we take |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 | BSSID | SA | DA | the transmitter (addr2) is a station; its BSSID is **addr1** |
| 0 | 1 | DA | BSSID | SA | the addr1 device is a station; its BSSID is **addr2** |
| 0 | 0 | DA | SA | BSSID | skipped — dominated by beacons, where addr3 is the AP's own address |
| 1 | 1 | RA | TA | DA | skipped — WDS/mesh, no station-to-BSS meaning here |

Both `trackWifiDevice` call sites (also `wifi_sniff.cpp`) pass their case's BSSID, so a tier-1 device (heard only as `addr1`, §15)
gets an association too — which is exactly the sleepy-camera case that tier 1 exists for.

**Why only three bytes.** At the time `WifiDev` sat in two arrays of `kWifiDevSlots` (the live table and the
`devSnap` snapshot buffer, removed in v1.11 - today it is one array, so a byte costs 64), so a byte added to it
cost 128 bytes of static RAM. The struct was 64 bytes with three
bytes of interior padding; the full 6-byte BSSID would have rounded it to 68 and cost 512 B, against ~420 B
of headroom under the rule in §16. Reordering the fields so the `uint32_t` leads packs it to **exactly 64
bytes with no padding**, which is where `apSuffix[3]` now lives — measured cost zero. A
`static_assert(sizeof(WifiDev) == 64)` holds the line; if it ever fires, re-read the packing note in
`devices.h` rather than just bumping the number.

**Completing the join.** Three bytes is not a BSSID, so the host finishes the work in `_resolve_parents()`:
a station is attributed to an AP only when that AP is in *our own* table and its low 24 bits match, and
never when two known APs share those bits (the join resolves to nothing rather than guessing). A station on
an AP we have not heard shows no association at all. `dca` (handled in `host_proto.cpp`, acting on `startDeauthTargeted()` in `deauth_diag.cpp`)
is then always sent the AP's **full** BSSID, read
from that AP's own row — the suffix is a join key and never goes back to the device.

Same standard as the surveillance OUIs in §15: a match is evidence, and the UI says "on &lt;ssid&gt;" only
for a BSS we independently heard.

## 18. Spectrum mode: 2.4 GHz energy detection + unexplained-energy flagging (1.7)

Every other mode measures RF only as its radio *decodes* it: a Wi-Fi/BLE/15.4 RSSI exists only on a frame
that demodulated, and `computeBusyScore()` is a packet-activity proxy, not a noise floor. `spec` mode is
different — it reads **raw channel energy, no decode**.

### The one real energy primitive on this board
The prebuilt core's 802.15.4 driver exposes `esp_ieee802154_energy_detect(uint32_t duration)` (duration in
16 µs symbols) with the weak callback `esp_ieee802154_energy_detect_done(int8_t power)` giving a detected
level in **dBm**. `ieee154.cpp` already included `<esp_ieee802154.h>`, so nothing new had to be pulled in.
It covers only the 15.4 channels **11–26 (≈2402–2480 MHz, 5 MHz bins)** — which overlaps all of 2.4 GHz
Wi-Fi (ch 1–13), BLE and Zigbee/Thread, plus most proprietary 2.4 GHz traffic. **There is no equivalent for
Wi-Fi/5 GHz**: `rx_ctrl.rssi` only ever arrives attached to a decoded packet, so a true swept analyzer is
2.4 GHz only. This is the single most important constraint on the feature.

### Firmware path
`BAND_SPEC` reuses the 15.4 radio and its 11–26 channel set (`chanEnabled()` returns `is154(idx)`), but
`startSpectrum()` powers the radio **without** `set_promiscuous`/`receive()` — so `esp_ieee802154_receive_done`
never fires. Per channel, `applyChannelIdx()` calls `edReset()` then `edKick()` (one energy-detect window);
`esp_ieee802154_energy_detect_done()` folds `power` into a spinlock-guarded accumulator (sum/min/max/count).
**Re-arming the next window must happen from the loop task, not the done callback** — arming from inside the
ISR callback silently fails and yields exactly one sample per dwell (measured). So the callback only sets a
`s_edReady` flag and `serviceSpectrum()` (called from `Bandwatch_Loop()`, ~every 2 ms) kicks the next ED.
That gives **~30 samples per ~120 ms dwell** (each a `kEdDurationSym` = 2 ms window; see Speed below), which is
what lets `edMax` catch bursty emitters rather than a single lucky/unlucky snapshot. **The 2 ms window is
load-bearing:** the original 128 µs window listened only ~5 % of the dwell and missed bursty Wi-Fi almost
entirely — a strong but idle AP (beacons ≈ 1 % airtime) produced *no* energy peak, so the spectrum did not
track the Wi-Fi band. At 2 ms (~50 % coverage) a window usually overlaps a beacon/burst, and the energy peaks
line up with the actual APs (verified: ch12 AP at −71 dBm → energy peak at 2465 MHz; ch6 AP → peak at 2437). `finishDwell()` snapshots
min/mean/max dBm and the sample count into `ChannelState.edMin/edMean/edMax/edSamples` and maps the peak onto
the shared 0–100 bar/LED range via `edDbmToScore()` (clamped to `kEdFloorDbm`/`kEdCeilDbm`, −95…−20). The
accumulator lives in `ieee154.cpp` behind `edReset/edKick/serviceSpectrum/edSnapshot`; `specRunning` gates the
mode exactly like `r154Running` gates 15.4 (`advanceChannel`/`hopIfNeeded`/`setBandMode`). RAM cost is ~6 B ×
`kChannelCount` in `ChannelState` (not the packed `WifiDev`) plus a handful of accumulator ints — trivial.

### Serial
`sendSweep()` appends four energy fields to each row **only in `spec` mode**:
`[ch, score, frames, bytes, strong, unique, state, edMin, edMean, edMax, edSamples]` (all dBm except the
sample count). (Historical: the per-row energy fields were later superseded by the fine-spectrum `fs` line, and the sweep budget is
derived per channel since v1.19.3.) Only 16 channels sweep here, so the longer rows stayed inside the budget;
Wi-Fi/154 rows are byte-for-byte unchanged. `edSamples` doubles as a confidence indicator.

`sendDwell()` also appends `"e":[edMin,edMean,edMax,edSamples]` per dwell **only in `spec` mode**, so the
dashboard fills bars in and walks the current-frequency marker live (like the LCD) between the full-sweep `s`
messages rather than refreshing the whole chart once per sweep.

**Speed and resolution.** The spec dwell (`kEdDwellMs`, 60 ms vs `kDwellMs` 220 ms) shortens the sweep, but
hopping stays on the 120 ms LVGL UI timer (`hopIfNeeded` in `uiTimerCb`), so the effective dwell is ~120 ms
and a full 16-channel sweep is ~2 s. **Do not drive the hop faster than this.** An attempt to hop every 60 ms
from `Bandwatch_Loop()` (~2 ms cadence) was reverted: the radio needs ~>100 ms to settle after
`esp_ieee802154_set_channel()`, and hopping sooner made energy detection return a stuck ~−40 dBm on *every*
channel — a flat reading that did not track real occupancy (verified against the Wi-Fi busy map: channels the
Wi-Fi radio showed empty still read −40). At the ~120 ms dwell the floor sits near −110 dBm with 40+ dB of
real structure that lines up with actual traffic.

**Fine resolution — off-grid tuning (1.8).** The public `esp_ieee802154_set_channel()` takes a channel number
(11–26) only, but the HAL exposes `ieee802154_ll_set_freq(uint8_t)` (inline in `hal/ieee802154_common_ll.h`)
which writes the synth's 7-bit `channel.freq` register = MHz−2400. So spec mode no longer sweeps the 16
channel entries — it sweeps **2400–2483 MHz in `specStepMhz` (1/2/5 MHz, default 2) steps** via `edSetFreqMhz()`
(`ieee154.cpp`), giving up to 84 bins and covering the band edges the channel grid can't reach. This is below
the public API but it is a documented inline function, not a vendor-blob patch like the §9 deauth offsets, and
off-grid reads were verified against known APs (ch6 AP → peak at 2435–2437 MHz; clean −111 floor between
emitters). In `bandwatch.cpp`, spec mode repurposes `currentIdx` as the bin index into `specFine[kSpecMaxBins]`
(a lean per-bin struct, ~0.5 kB) and `advanceChannel`/`finishDwell` have early spec branches that bypass the
channel machinery. **Still 2.4 GHz only** — the 15.4 radio is 2.4 GHz silicon; no register reaches 5 GHz.
Serial: `sendSweep()` emits `{"t":"fs",...,"bins":[[min,mean,max,n],...]}` (frequency = lo+i·step, not sent
per bin) once per sweep, `sendDwell()` emits a light `{"t":"fd","mhz":...}` per dwell for the dashboard's live
cursor, and `specstep` sets the step. More bins = slower sweep (~120 ms/bin), so 84 bins ≈ 10 s, 42 ≈ 5 s.

### Host: flagging unexplained energy (not identifying protocols)
The radio **cannot demodulate an unknown protocol** — it only knows 802.11/BLE/802.15.4. What the host does
is correlate: `_unidentified()` (in `bandwatch_host.py`) maps each decoded emitter to the frequency span it
occupies — Wi-Fi 2.4 ch *C* → 2412+5·(*C*−1) MHz ±11 MHz, BLE adv → 2402/2426/2480, 15.4 ch → its own bin —
using only devices seen in the last `SPEC_KNOWN_AGE_S` (120 s). It estimates the **band-wide noise floor** as
a low percentile across all bins/recent sweeps (`spec_hist`), then flags any bin whose peak sits
`SPEC_FLOOR_MARGIN` dB above that floor with duty ≥ `SPEC_MIN_DUTY` and whose center frequency no known span
covers. The floor is band-wide on purpose: a per-bin min makes a *continuously-on* emitter look quiet
against its own flat history.

The honest caveat, stated in the UI: one radio can't decode and energy-scan at once, so "known" is whatever
the Wi-Fi/BLE/15.4 modes last saw — sweep those first, then switch to `spec`. A hit means *energy with no
decoded explanation* (evidence of an emitter — proprietary link, video sender, RC/drone controller, microwave,
interferer), **not** a device identification. Same standard as §15/§17: a match is evidence, and the wording
says so.

### UI
LCD: `PAGE_SPECTRUM` (`buildSpectrumPage`/`refreshSpectrum` in `lcd_ui.cpp`), one bar per 15.4 bin scaled
from `edMax` via `edDbmToScore()`, available only in `spec` mode (`pageAvailable`). Dashboard: the spectrum views
in the main column (a dBm bar chart, a client-side waterfall (`<canvas>`, one row per completed sweep), and the
unexplained-energy list from `s.unidentified`), plus the `explain` control (§5).

## 19. Live LCD mirror over serial (1.13)

A `mirror 1|0` command streams the device's 172×320 LCD to the host so the dashboard can show the real
screen, not just the decoded data. Default **off**, not persisted (like `park`/`hunt`): a reboot never comes
up mirroring.

### Why it piggybacks the flush callback
LVGL renders in `LV_DISPLAY_RENDER_MODE_PARTIAL` into one ~5 KB draw buffer (`buf1`, `LVGL_WIDTH*LVGL_HEIGHT/21`).
Every repaint calls `Lvgl_Display_LCD(disp, area, px_map)` (`LVGL_Driver.cpp`) with the dirty rectangle and its
RGB565 pixels on the way to the panel. The mirror taps exactly there — when `g_mirror` is set, the callback
also calls `mirrorOnFlush(x1,y1,x2,y2,px_map)` (`host_proto.cpp`), which emits one line:

```
M <x> <y> <w> <h> <base64 of w*h*2 bytes, RGB565 little-endian>
```

So there is **no extra framebuffer in RAM** (a full frame is 110 KB, more than free heap): it sends pixels
that already flow through the flush path. `px_map` is LVGL's native buffer (`LV_COLOR_16_SWAP` is 0), so the
host decodes each pixel as a little-endian `uint16` — which is the colour the panel shows, since the device
displays LVGL's intended colours correctly. `LCD_WriteData_nbyte` uses `SPI.writeBytes` (no swap, no mutation),
so reading `px_map` after the panel write is safe.

### Pacing, frames and repair (1.18.1; replaces the 1.13 full re-scan)
All dirty regions in one `lv_timer_handler()` flush **synchronously** back-to-back, and `Serial` is non-blocking with
an 8 KB TX buffer (`setTxTimeoutMs(0)`, `CLAUDE.md` rule 6) - it never drains mid-handler. Three mechanisms keep the host's copy
honest:

1. **Row slices.** `mirrorOnFlush()` sends each flushed region as `M` lines of <= ~2 KB base64 (as many rows as fit
   1536 raw bytes), each a whole line. A flush chunk is up to ~15 full-width rows (~7 KB base64); sent as one line
   it fitted only a nearly empty buffer, so on busy pages nearly every chunk dropped. Slices go out while there is
   room, and only the rows that did not fit are noted as dropped.
2. **Repair rectangle.** A dropped slice is unioned into **one** pending repair rectangle (`mirX1..mirY2` in
   `lcd_ui.cpp`, a bounding box - 8 bytes, no per-region list). `serviceMirror()` (once per loop) re-invalidates that
   rectangle top-down in ~10-row strips, as many per LVGL refresh as the TX buffer has room for (`mirInflightB`
   counts bytes invalidated but not yet flushed; a 250 ms guard stops a refresh that never ends from stalling it).
   LVGL re-renders the *current* pixels there and the flush path emits them; a strip that drops again just goes back
   in. `mirror 1` (`mirrorRequestFull()`) starts with the whole screen as the rectangle - the first full frame is the
   same code. The 1.13.1 design re-scanned the whole screen after any drop, which kept a busy page's mirror a mix of
   strips from different moments.
3. **Frame markers.** The flush callback passes `lv_display_flush_is_last(disp)`; after the last region of a refresh
   `mirrorOnFlush()` emits `MF <seq> <complete>` (`seq` a wrapping u16). `complete = 1` means nothing in this refresh
   dropped **and** no repair is outstanding - the host's copy then equals the panel exactly.

Measured (1.18.1, 20 s windows): on the Wi-Fi pages ~57% of frames arrive complete. On the **BLE Devices page none
do**: it re-sorts and repaints all 12 rows every 500 ms, ~110 KB of base64 per repaint (~220 KB/s), more than the
USB serial link carries, so the repair can never catch up. That is a bandwidth limit the framing makes visible,
not something it can fix; the dashboard says "repairing" there.

### Host + dashboard
`handle_mirror()` (`bandwatch_host.py`) blits each `M` region into a **back buffer**; `handle_mirror_frame()` copies
it to the served framebuffer (`screen`, bumping `screen_seq`) only at an `MF` marker. A complete frame is published at
once; a torn one is held back unless nothing has been published for `MIRROR_HOLD_S` (0.5 s) - a page faster than the
link would otherwise freeze, so it then shows the newest frame available. Firmware without markers is detected (a run
of `MIRROR_LEGACY_REGIONS` regions with no `MF`) and published per region as before; a lost final marker is covered by
publishing a quiet back buffer after `MIRROR_STALE_S`. `GET /screen.bin` serves the published frame (with
`X-Screen-W/H/Seq` headers); `GET /api/screen` returns `{on, seq, w, h, framed, complete, ...frame counters}` (`on` =
a mirror line within 3 s), and the v2 canvas shows *in sync* / *repairing dropped regions* / *unframed*. The
**v2 dashboard** (`dashboard2.html`) has a *Start mirror* card: the toggle POSTs `mirror 1|0`, a 4 Hz poll
watches `seq`, and on a change it fetches `/screen.bin` and paints it onto a `<canvas>` (RGB565 LE → RGBA,
`image-rendering: pixelated`). `sendHello` carries `"mir"` so the button reflects the device state on load.

The mirror panel lives in the v2 **left control rail** (the 172×320 canvas fits its width). Alongside it are
**Page ‹ ›** buttons that POST `page prev|next`, which the device handles with `stepPage()` (`lcd_ui.cpp`) —
the same action as a BOOT tap (`showPage(currentPage ± 1)`, skipping pages that don't apply to the mode, and
clearing any mode splash). This lets you drive the LCD from the browser and hold on a page. It matters because
the serial link can't keep up with a continuously-repainting page: in `spec` mode the bars, and in BLE the
device list, redraw several times a second, so more pixels change per second than the link carries and frames
stay "repairing". Stepping to a mostly-static page (Devices, System, a parked channel) lets it settle to a clean
frame. This is a bandwidth limit, not a bug — a full 172×320 frame is ~147 KB of base64 and USB-CDC is the
cap.

## 20. Event log to SD (C4, 1.18)

The untethered "what happened on this walk" record (ROADMAP C4, delivering 1.6.2 and 1.6.4). `events 1` arms it,
`events 0` disarms; the flag is persisted in NVS (`settings.cpp`, key `events`) and re-applied in
`Bandwatch_Init()`, so a device switched on in a pocket logs without a host. *Armed* is independent of the card:
it arms with no card in the slot and attaches when one appears.

### What is logged
`/events.csv`, header `epoch_ms,up_ms,kind,id,rssi,ch,radio,extra`, one row per event:

- `kind = surv` — a surveillance-OUI match (built-in `kSurvOuis`, then `/surveil.csv`), the first time a device is
  seen this session (a 32-entry seen set; past 32 surveillance devices a re-created slot can log again). `extra` =
  category name, plus ` (dest only)` for a tier-1 (addr1-only, §15) sighting.
- `kind = new` — a globally unique MAC not in the card's `/seen.csv` baseline. `extra` = `dest only` for tier 1.
  **Randomized MACs never count as new** (Wi‑Fi locally-administered bit, BLE random address type): rotating
  privacy addresses would be "new" every few minutes. They are still logged as `surv` when they match.
- `epoch_ms` is empty until the host has sent `time` (no RTC); `up_ms` is always there. `radio` is `wifi` or `ble`;
  `ch` is the Wi‑Fi channel (0 for BLE). 802.15.4 nodes are not logged.

Events come from slot *creation*: the Wi‑Fi and NimBLE paths call `eventFlag()` under `g_devMux` when
`trackWifiDevice()` / the BLE tracker create a device slot; it copies MAC/RSSI/channel/category/flags into a
16-entry queue (`g_evtQ`) and returns, dropping and counting on overflow. Everything else runs on the loop task in
`serviceEvents()`: dedup, classification, row formatting, flushing.

### The device register: `/seen.csv` v2
`/seen.csv` is the novelty baseline *and* a register of every globally unique device the log has met on this card.
Pure row logic (format, parse, sanitize, top-K selection, the line reader) is in `seen_row.h`, which has no Arduino
dependency and is tested natively; the file handling is in `events.cpp`.

**Format.** A header line, then one fixed-width row per device, 83 bytes with the newline:

```
# bandwatch seen v2: mac,type,label,first,last,sessions
aa:bb:cc:dd:ee:ff,ap ,HomeNet                         ,1790000000,1790001234,00007
00:1b:63:00:00:01,sta,                                ,0000000000,0000000000,00001
```

| field | offset | width | content |
| --- | --- | --- | --- |
| mac | 0 | 17 | lowercase, colon-separated |
| type | 18 | 3 | `ap ` (beacon/probe response seen), `sta`, `ble`, or `?  ` (unknown: a row converted from v1) |
| label | 22 | 32 | SSID or BLE name when the row was written; control characters, `,` and `"` become `.` (rule 9); cut or space-padded to 32 bytes; UTF-8 kept |
| first | 55 | 10 | epoch seconds of the first sighting, zero-padded |
| last | 66 | 10 | epoch seconds of the latest recorded sighting |
| sessions | 77 | 5 | sessions the device was seen in, zero-padded, saturating at 99999 |

**No clock:** there is no RTC, so `first`/`last` are `0000000000` for a device met before any host sent `time`.
An update without a clock leaves `last` as it was (never writes a 0 over a real time) and still counts the session;
`first` is never rewritten, so a 0 stays "unknown" even after a clock arrives. Readers should skip `#` lines and trim
the padding. A *session* is one attach of the card with the log armed: arming, a boot with the log armed, or a
card re-inserted or swapped.

**New device** (a globally unique MAC not in the RAM set): its `new` row goes to `/events.csv` as before, and an
entry `{mac, type, epoch}` waits in `newDevs` (48 × 12 B) until the next flush appends its row with
`first = last = ` the time it was classified, `sessions = 1`. The type is AP/station from the live Wi-Fi table
(BLE from the event flags); the label and a late AP flag are looked up in the live tables *at flush time*, so a
beacon or scan response that arrived after the slot was created still names it. A slot already evicted by then
leaves the type known at creation and an empty label.

**Known device seen again.** The RAM set holds, per entry, a 30-bit MAC hash and two flag bits: *dirty* (seen since
its row was last written) and *counted* (its session count was already raised this session). Two sources set
*dirty*: a slot re-created for a known MAC (`eventFlag()`, as before), and `touchLive()`, which every 60 s marks each
known, non-randomized device in the Wi-Fi and BLE tables whose `lastMs` moved since the previous sweep - without it,
a device that keeps its slot all session would never be refreshed (`eventFlag()` only fires on slot creation).
**Register pass** (`regStart()` / `regStep()`): when entries are dirty and 5 min (`kRegisterMs`) have passed since
the last pass, a flush opens `/seen.csv` `r+` and the loop walks it in slices of at most `kSdBudgetUs` (8 ms, the
capture drain's budget), so hopping and the LCD keep running. For each row whose entry is dirty it patches the line
buffer - `last` = now (kept without a clock), `sessions` + 1 if not yet counted, and, for a `?` row of a device that
is still in the live tables, its type and label - writes the 82 bytes back over the row (same width, nothing moves),
seeks back to the read position, and clears *dirty* / sets *counted*. Only the first row of a MAC is updated if a
v1 history left duplicates. The card stays mounted only for the pass; `sdUnmount()` will not unmount under it
(`eventsHoldsCard()`), `sdcap` and `sdread` make it step aside (`eventsReleaseCard()`; entries it had not reached
stay dirty for the next pass), `sdrm` and `seengen` refuse while it runs (`eventsFlushing()`), and an I/O error ends
it like any failed write (`cardLost()`). `events 0` finishes the running pass and runs a final one (blocking,
bounded by one read of the file), so a disarm leaves the register current; a power cut loses at most the last 5 min
of `last`/`sessions` updates. Why every 5 min and not every flush: the pass reads the whole file (up to ~340 KB) and
holds FATFS (~30 KB) for its length, which in BLE mode sits near the scan floor (rule 11). It is skipped below
24 kB free heap like a rotation; the dirty bits simply wait.

**Loading** (`loadBaseline()`, on attach): the RAM set holds every row when there are at most 2048 (one pass), else
the **2048 seen most recently** (`last`): a first pass keeps the 2048 largest `last` values in a min-heap built in the
baseline array itself (`topkPush`), its root is the threshold; a second pass loads every row above it and, of the rows
*at* the threshold (ties - every row is one when there was never a clock), the *last* ones in file order via a small
ring (`RingLoad`), which is what v1 did (newest appended). The file size picks the mode up front. `kBaseCap` = 2560
leaves room for 512 new devices this session (past that a MAC is still logged as new but not remembered in RAM). A
hash collision makes a new device look known (a missed row), never the reverse; with 30 bits it is ~2.4e-6 per lookup.

**Rotation.** When an attach finds more than `kSeenRotate` = 4096 rows, `rotateSeen()` renames `/seen.csv` to
`/seen.old.csv` (replacing an older one) and copies back **the same 2048 rows the RAM set just loaded**
(`selectKeep()` makes the same choice as the load, ties included), so novelty does not change; the header is
rewritten first. It logs `{"t":"log","msg":"seen.csv rotated: N -> M in R ms (attach A ms)"}`: R is the copy, A
the whole attach (all passes) - the loop task is busy for A, so that is the number BACKLOG V5 measures. Same
safety as before: no MAC buffer (one line buffer per pass), two FATFS files open, skipped below 24 kB free heap and
retried on the next attach; if the copy fails the old file is renamed back; a power cut mid-copy leaves the whole
history in `/seen.old.csv`. Cost: a file at the threshold is ~340 KB (83 B a row, was 18 B in v1), so an attach at
2049-4096 rows reads it twice and a rotation three times plus a ~170 KB write - all blocking on the loop task.

**v1 compatibility.** A v1 file (one bare MAC per line, no header) - or a v2 file to which an older firmware appended
bare MACs - is converted on attach (`convertSeen()`): every row is copied to `/seen.new.csv` (v2 rows verbatim, v1
lines as `?`, empty label, `0`/`0`, 1 session), then `/seen.csv` is removed and the new file renamed over it, with
`{"t":"log","msg":"seen.csv converted to v2: N rows in M ms"}`. A power cut between the remove and the rename is
finished by the next load (no `/seen.csv` but a `/seen.new.csv`); a leftover partial `/seen.new.csv` next to a
`/seen.csv` is deleted. If the conversion cannot run (heap, card full), the file is loaded as it is - v1 lines count
as rows - and the conversion is retried on the next attach. The register then fills in `?` rows as those devices are
met again. Going back to a v1 firmware still works: its reader takes the MAC prefix of each v2 row and skips the
header.

**`seengen <n>`** (diagnostic, like `sdprobe`; not exposed by the host): writes `n` (1..10000) synthetic v2 rows -
MACs `00:1b:63:xx:xx:xx` (a real OUI, globally unique, so they count), types cycling ap/sta/ble, labels
`net-NNNNN`/`tag-NNNNN`, `last` spread over the 30 days before now (a fixed 2026 date without a clock), `first` up
to 30 days before that, 1-20 sessions - and replies `{"t":"ack","cmd":"seengen","n":N,"ms":M,"ok":1}`. The real
register is parked first: `/seen.csv` -> `/seen.bak.csv` (an empty one if there was none) and `/seen.old.csv` ->
`/seen.old.bak.csv` (the rotation would replace it); parked files are never overwritten, so a second `seengen` only
replaces the synthetic file. `seengen 0` deletes the synthetic `/seen.csv` and `/seen.old.csv` and puts both back
(err `seengen: no /seen.bak.csv to restore` when nothing is parked). Refused (`seengen: busy - ...`) while `sdcap`,
`sdread` or an event-log flush/register pass has the card. Blocking (~1 s per few thousand rows, yielding every 64
rows). With the log armed the new file is loaded at once (an attach); disarmed, the next `events 1` or card
insertion is the attach. Measuring V5: `events 0`, `seengen 5000`, `events 1` -> `seen.csv rotated: 5000 -> 2048 in
R ms (attach A ms)` before the ack, `ev.file` 2048; then `events 0`, `seengen 0` (`T14SeenGen` does exactly this).
Measured on v1.20.0: R = 582 ms, A = 1,312 ms for 5000 rows.

### Buffering and flushing
Rows go into a 2 KB heap buffer (~25-30 rows); overflow is counted in `drop`, not hidden. The card is **not**
kept mounted (rule 11): it is mounted just long enough to append, when 16 rows are waiting, 48 new devices are pending
for `/seen.csv`, or 60 s have passed with anything pending (plus the length of a register pass, every 5 min at most,
above). `/events.csv` rotates at 1 MB to `/events.old.csv`
(one previous file kept). Flushes wait while `sdcap` records or an `sdread` runs — the card belongs to them —
and rows keep buffering meanwhile.

### Missing, pulled or swapped card
- **No card:** novelty is suspended (each skipped check counted in `wait`), because with no baseline every device
  would look new. Surveillance rows still buffer. A mount is retried every 30 s (`kRetryMs`; an `SD.begin()` with
  no card blocks the loop briefly, so not every pass), and immediately when presence goes to inserted
  (`eventsNudge()` from `sdSetPresent()`, §12).
- **Failed mount or write:** the card is marked lost (`err` +1, `card: 0`), rows stay buffered, and `SD.end()` is
  forced unless a capture/`sdread` owns the card. The baseline is invalidated: the next successful mount reloads
  `/seen.csv` from *that* card, which may be a different one, and re-inserts the session's still-pending new MACs
  so they are not counted as new a second time.
- `events 0` makes one best-effort flush; rows that could not be written are freed and counted in `drop`.

### Status line
`ev` = `{on, card, base, file, written, pending, surv, new, drop, err, wait}` (§4) rides on `hello`, the `events`
ack, and a `{"t":"ev"}` line every 5 s while armed, in every mode. The dashboard shows it with an arm/disarm
control; the card's file list (§12) offers download and delete for every file. The host's `sdread`/`sdrm` guard
accepts the card's text files (`CARD_TEXT_FILES`: `events.csv`, `events.old.csv`, `seen.csv`, `seen.old.csv`,
`seen.bak.csv`, `seen.old.bak.csv` - the register `seengen` parks -, `surveil.csv`) besides the pcap naming scheme. Deleting `seen.csv` while armed starts novelty over (§12).

### `/surveil.csv` (1.6.4)
Up to 64 extra OUIs, one `AA:BB:CC,<category 1-7>` per line (categories as `kSurvName` in `surv_ouis.h`),
read once at boot by `sdProbeAtBoot()` → `loadSurvExtra()` into heap (absent file = nothing allocated).
`survLookup()` checks the built-in table first. A changed file takes effect on the next boot. Same honesty rule as
§15: a match is evidence, not identification.

### Memory
Nothing while off. Armed: ~12.9 KB heap (baseline 10,240 B + rows 2,048 B + pending appends 48 × 12 = 576 B), plus
FATFS (~30 KB) only for the duration of a flush or register pass, and during a pass its state (~0.3 KB: one 256 B
read buffer) and one open file (~4.3 KB) - less than `sdcap` holds, and never alongside it. The register's flags
live in the two low bits of each baseline hash instead of two 320 B bitmaps, and nothing else is kept per device in
RAM (no counters, no row offsets: the pass finds rows by reading). v2 vs v1: +288 B heap while armed (the pending
appends grew from 6 to 12 B to keep type and time), +16 B static (77,184 -> 77,200 B). Static: the queue and counters
(§16).

### Measured on hardware (1.18 session)
First flush at 16 rows; correct CSV with epoch timestamps; `seen.csv` at 31 entries after about 80 s. Card pulled
mid-`sdcap` and re-inserted: no rows lost (§12). The re-insert of pending new MACs into the reloaded baseline was
added *after* that test, because `base` read 32 instead of about 52 — it has not been re-run since.

### Open
- LED blip on a new/surveillance event: decided and shipped in 1.19 together with C10 and 1.6.1 (§21). A `new`
  row blips white; a surveillance device blips orange whether or not this log is armed.
- The presence probe has no open items (card out reads `0xFF`, card in `0x01`, removal detected within ~4 s;
  §12). Hot-pulling the card can intermittently reset the board over USB (`rst: usb`, a hardware effect, §12).
- Measured (v1.19): free heap at peak load with the log armed - `events 1` + `cap 1` + `sdcap 1`, `both` band, every
  LCD page - bottoms at **25,172 B** (Overview), ~0.6 kB above the 24 kB floor that `ensureCapRing()` sizes the ring
  to keep.
- Measured (v1.20.0): `seengen 5000` 522 ms; rotation 5000 -> 2048 **582 ms**, whole attach **1,312 ms** (measured v1.20.0, 2026-10-08, `T14SeenGen`). The loop task is busy for the
  attach (LCD and hopping pause ~1.3 s), once per rotation, so no chunking for now. Not yet seen on the board: a
  real v1 file converting, and a known device's row rewritten in place by the register pass.

## 21. LED alert blips (D1: 1.6.1 alerting, C4 novelty, C10 permit-join; 1.19)

The single WS2812 carries one *permanent* state chosen by `driveLed()` (`lcd_ui.cpp`): deauth blink > hunt distance >
record pulse > busy score. An alert is not a fifth permanent state: it **wins the LED for 300-500 ms with a distinct
pattern, then yields back** - `driveLed()` repaints on its next 120 ms UI tick. `led_alert.cpp` owns it.

### Patterns (50 ms slots)

| Kind | Trigger | Pattern | Colour | Priority |
| --- | --- | --- | --- | --- |
| `surv` | first time this session a device with `surv != 0` gets a slot (Wi-Fi or BLE) | double flash, 100 on / 100 off / 100 on / 200 off (500 ms) | orange `{255,120,0}`, 100 % | 3 |
| `join` | an 802.15.4 node's permit-join bit (`Dev154.flags` bit2) first becomes set | same double flash | purple `{150,0,255}`, 100 % | 2 |
| `new` | the event log classifies a `new` row (`serviceEvents()`; needs `events 1` and a card baseline) | single blink, 150 on / 150 off (300 ms) | white `{210,210,210}`, 60 % | 1 |

The trailing dark slots make the end of a blip readable even when the permanent state is the same colour (orange
busy score, orange hunt distance).

### Priority and rate limit
- **An active deauth always keeps the LED.** `driveLed()` checks `deauth.active` before `ledBlipActive()`, and
  `serviceLedAlerts()` drops a running blip and every pending kind while a deauth is active. Alerts during a deauth
  are **dropped, not queued**: a blip minutes later would point at nothing. A blip may override hunt, record and
  busy.
- **At most one blip per 2 s** (`kGapMs`, measured start to start). Kinds noted in the meantime collect as a bitmask
  and the highest one wins when the gap ends. A note of the same or lower priority than the blip whose gap it falls
  in is absorbed by that blip, so a burst gives one blip (two at most, when a higher-priority hit follows a
  lower-priority blip) - never a strobe.

### Plumbing (and why it is safe)
- **Surveillance is independent of the C4 event log** (`g_eventsOn` is not consulted). `trackWifiDevice()` /
  `trackBleDevice()` call `ledNoteSurv(mac)` (IRAM, already under `g_devMux`, no heap/Serial) on slot creation when
  `d.surv != 0`; it pushes a 32-bit FNV hash into a 4-entry ring (overflow sets a flag that still blips). The loop
  drains the ring under `g_devMux` and dedups against a 16-entry seen ring, so a device blips once per session (a
  17th distinct surveillance device can evict an old one, which may then blip again - harmless).
- **Permit-join**: the 802.15.4 RX path is a true ISR, so `track154()` only sets `g_ledJoinFlag` when
  `(flagBits & 4) && !(d.flags & 4)`. The bit is sticky per slot (ROADMAP C10's open question), so a node blips
  once when first seen permitting, not at beacon rate; it blips again only if its slot is evicted and recreated.
- **New**: `serviceEvents()` (loop) calls `ledAlertNote(LED_ALERT_NEW)` beside `g_evStats.fresh++`.
- `serviceLedAlerts()` runs every loop pass (finer than the 120 ms UI tick) and writes the LED only on on/off edges.
- `alerts 0` clears pending kinds and stops new ones; the seen ring keeps filling so turning alerts back on does not
  replay old hits. `ledtest` bypasses the rate limit and the switch, not the deauth rule.

### Settings and cost
`alerts` is an NVS key (`settings.cpp`); an absent key reads as 1, so the schema stays 1 and older NVS keeps
working. Static RAM +104 B (77,288 -> 77,392 B): the two rings, pattern state and the flag. No heap. Hello's serial
budget went 900 -> 920 for the new field.

### Verify on hardware
`ledtest surv|join|new` shows each pattern on demand. A real surveillance hit: a device from `kSurvOuis` (or a test
OUI added to `/surveil.csv`) in range, `band 2.4g` - one orange double flash when it first appears. Permit-join: a
Zigbee coordinator with joining opened, `band 154`. With `deauth <bssid>` running, `ledtest surv` acks `"shown":0`
and the red attack blink never breaks.

## 22. Patrol mode (C5, 1.20)

One radio cannot decode and measure energy at once, so the spectrum's "explained" sources (§18) are only as fresh as
the last Wi‑Fi/BLE/15.4 sweep. Patrol walks the modes on its own so a board left on a desk keeps them fresh: `patrol 1`
runs `spec` 30 s -> `both` 40 s -> `ble` 20 s and repeats; `patrol <mode>:<sec>,...` sets 2-6 legs of 5-600 s.

### How it works
- **State**: `Patrol` in `bandwatch_core.h` (leg table, current leg, leg start, cycle count, the band it started from,
  28 B), one instance in `bandwatch.cpp`. `servicePatrol()` runs from `Bandwatch_Loop()`; `hopIfNeeded()` (the UI
  timer, same task) sets `g_dwellEdge` after each finished dwell.
- **Hand-off on a dwell boundary**: when a leg is due, `servicePatrol()` waits for the next `g_dwellEdge`, so the last
  dwell of a leg is scored whole (at most one dwell late: 220 ms, 60 ms in spec). BLE has no dwells and a sweep mode
  that is not on a channel never finishes one, so those hand off at once; a +1 s backstop covers a dwell that never
  completes. The `{"t":"pt"}` line reports `left` 0 meanwhile.
- **Each hand-off is a normal mode change**: drop any park (a leg sweeps), `setBandMode()` (so `releaseCapture()`,
  `stopDeauth()` and the radio teardown/bring-up run exactly as for `band`), `showModeChange()` (the page + mode
  card a `band` command shows), a `patrol: leg i/n <mode> <s> s` log line, `sendHello()` and a `pt` line. The leg
  clock starts after the new radio is up, so BLE's bring-up is not charged to its leg. The dashboard sees only
  ordinary mode changes, so its cached channel views collapse and return as they do for a click on a mode.
- **v1 has no capture**: every hand-off releases the ring and the pcap link type differs per radio, so `patrol` is
  refused while a USB or SD capture runs, and `cap 1` / `sdcap 1` are refused while it walks. Hunt and deauth park
  the radio, which a hand-off would undo, so they exclude patrol the same way (`huntssid`, C7, is refused by its
  command word). Per-leg capture is the documented stretch.
- **Manual control wins**: any `band` command (even onto the current leg's mode) and a BOOT-hold mode walk stop the
  patrol; `patrol 0` stops it where it is. The event log (`events`) keeps running across legs.
- **Not persisted**: a reboot comes up not patrolling. While one runs, `saveSettings()` stores the band the patrol
  started from, so another setting's save (e.g. `alerts`) does not persist a leg as the boot mode.
- **LCD**: the header's right label reads `patrol BOTH 23s` (mode, seconds left in the leg) on the Activity,
  Channels, Spectrum and Devices pages.

### Cost
Static RAM +56 B (77,184 -> 77,240 B): the `Patrol` struct, the dwell-edge flag, the status timestamp and the
command line buffer growing 48 -> 64 B. No heap. Hello's serial budget grew by `kPatrolJsonMax` (128 B); the `pt`
line is <= 139 B every 2 s, through `sendLinef()` (dropped whole when the TX buffer is short, rule 6).

### Verify on hardware
`patrol both:5,ble:5` and watch `hello` lines alternate `both`/`ble` with `pt.leg` 0/1 and `pt.cyc` counting up, and
`{"t":"pt"}` every 2 s in BLE too (`tests/device` T13). Free heap before/after each hand-off should stay within a few
hundred bytes (testing checklist 7). The LCD header counts the leg down. Not yet run on the board.

## 23. Hunt by SSID (C7, v1.20)

`huntssid <name>` hunts a network name instead of a MAC: "where is my mesh node" without knowing its BSSID. It is a
third hunt kind (`Hunt.kind` 2) beside MAC (0) and 802.15.4 key (1); one hunt runs at a time, and starting any hunt
replaces the running one (and releases a park the old one held).

**Matching.** The name is the rest of the command line, spaces included, 1..32 bytes. It goes through the same
`sanitizeText()` as an SSID off the air, then is compared exactly and case-sensitively (`huntSsidEq`) with the SSID
stored in the Wi-Fi table, in `trackWifiDevice()`, for **beacons and probe responses only** - an AP's data frames
and every client are ignored. An empty SSID (hidden network) never matches. `huntssid 0` stops, so a network
literally named `0` cannot be hunted; the host refuses that name (400) rather than send a stop.

**RAM.** No new static buffer: the name lives in `hunt.label` (33 B, already there for the LCD), which is only
rewritten when `kind` changes under `g_devMux`, so the RX callback can read it. The best AP's channel sits in the
struct's padding (`static_assert(sizeof(Hunt) == 64)`). The park state machine adds a few loop-task bytes:
77,184 -> 77,192 B static.

**The reading.** Several APs can share a name (mesh, extenders, 2.4 + 5 GHz radios). `noteHuntSsidHit()` keeps
`hunt.mac/ch/rssi/lastMs` on the strongest matching AP heard recently: the current one keeps the reading while it
is heard, a louder one takes it over at once, a quieter one only after the current one has been silent for
`kHuntSsidFreshMs` (15 s, longer than one `both` sweep). Every matching frame counts as a hit. The serial `h`
status (`[rssi, age_ms, hits]`) is unchanged, so old hosts keep working; the dashboard lists every matching AP
from the device table it already has (no extra protocol).

**The park (`serviceHuntSsid()`, loop task).** A MAC hunt parks only when given a channel; an SSID hunt derives it:
- On start, if the table already holds a matching AP, park on the strongest one's channel (last writer wins, like
  `hunt <mac> <ch>`). Otherwise keep hopping and park as soon as one is heard (SEEK).
- While parked only that channel is heard, so every `kHuntSsidRescanMs` (30 s), or as soon as the followed AP has
  been silent for `kHuntSsidLostMs` (5 s), the hunt releases the park for one full sweep
  (`(enabledCount() + 2) x dwell`) and re-parks on the strongest AP heard during it. That is how the park follows
  you from one AP to another on a different channel; it costs a few seconds of hopping per rescan.
- Someone else parking elsewhere (`park <ch>`, a deauth) takes the park for the rest of the hunt (YIELD); an unpark
  (`park 0`, a band change that disabled the channel) sends it back to SEEK, which re-derives the park. While a
  deauth runs the hunt never moves the park (that attack is pinned to its channel).

**Wire format.** The hunt ack and `hello` keep their shape; an SSID hunt puts the escaped name in `hunt` and adds
`"ssid"` with the same name. Both are budgeted with `jsonStrLen()` (rule 6): the ack goes through `sendLinef()`
(2 x 66 B worst case), `hello` adds the measured excess over its 25-byte hunt id to its room check.

**LCD.** PAGE_HUNT shows the name as the label, wrapped to two lines and then cut with "...": the label is 160 px
wide, a typical 32-char SSID measures 255-275 px at Montserrat 14 (fits two lines), and only extreme names
(32 x `W` = 504 px) lose their tail. The line above it reads `N APs  best ch X` (or `N APs known` before the first
hit). The Montserrat fonts are ASCII-only, so non-ASCII bytes in a name still render as boxes.

**Verify on hardware.** Two APs with one SSID on different channels: walk between them; the reading and the park
should follow the stronger (allow one rescan, up to ~35 s). A name that matches nothing: zero hits, hopping, no
crash. `park 6` during the hunt: the hunt leaves the park alone from then on.

## 24. BLE kick (`blekick`, v1.21)

The deauth equivalent for BLE. There is no BLE management frame to spoof, so instead of blasting a "leave" we
**take over the victim's connection**: Bandwatch becomes a NimBLE *central* aimed at one target MAC and repeats
connect -> hold ~600 ms -> disconnect with status **0x13** ("Remote User Terminated Connection"). A single-slot
peripheral (a beach speaker playing from its phone) drops or stutters the real peer each cycle. `blekick <mac>`
starts in BLE mode, `blekick 0` stops; an unparseable MAC or a non-BLE band also stops — exactly mirroring `deauth`.

**Why takeover.** Four families of BLE DoS exist (see the plan's prior-art table): advertising flood, **connection
takeover**, L2CAP data flood, and raw passive `LL_TERMINATE_IND` injection. Takeover is the only one doable with
stock NimBLE host APIs on this core — no raw PDU injection, no GATT/L2CAP client. Prior art: blue-deauth's "connect
flood" (same idea over BlueZ), btlejack's passive "jam" (injects a forged 0x13 `LL_TERMINATE_IND` with a coprocessor
board — prior art for the reason code, not the mechanism), bettercap #719 (still open in 2026: no BLE DoS shipped),
and the GHM DoS paper (DOI 10.18466/cbayarfbe.856119): one l2ping did nothing to headphones, ~7 parallel ones
disconnected them — that is the v2 "flood" mode if takeover alone doesn't bite.

**State machine (`ble_kick.cpp`).** `startBleKick(mac)` copies the MAC and resolves the peer address type from a fresh
`bleDevs[]` entry (default random) under `g_devMux`. `serviceBleKick()` paces it from `uiTimerCb()` at ~120 ms:
idle for `kBleKickGapMs` (700) -> `ble_gap_connect(BLE_OWN_ADDR_PUBLIC, peer, kBleKickTimeoutMs (4000), NULL)` with its
own GAP callback; connected and held for `kBleKickHoldMs` (600) -> `ble_gap_terminate(connh, 0x13)`. The callback runs
on the NimBLE host task under the same single-producer discipline as `bleGapEvent()`: shared fields only under
`g_devMux`, Serial outside. A dead-man's switch (`kDeauthMaxMs`, like deauth) auto-stops a forgotten attack, and a mode
change does too (`stopBleKick()` in `setBandMode`). Stop mid-connected must not hold the victim's slot until its
supervision timeout: `serviceBleKick()` terminates once for the `!active && st == 2` case.

**Protocol.** A `"bk"` member `[mac, state, kicks, fails]` (state ∈ idle/connecting/connected; null when stopped) rides
the BLE heartbeat (`sendBleStatus()`, budget raised 300 -> 360 B) and the ack — BLE mode has no dwell lines for a live
counter to ride (§14). Log lines are the on-hardware oracle: `blekick #N connected` per won connect, first refusal +
every 16th as `blekick attempt failed rc=<nimble>`. The Kick button sits on each BLE table row; the red attack blink is
shared with deauth.

**RAM.** `BleKick` packs to 28 B (field-ordered, `static_assert`). Static total 77,288 -> 77,312 B (+24).

**Open questions (honest failure modes).** A victim busy while paired *refuses* new peers: `kicks=0`, `fails` climbs —
that is the signal, not a bug; v2 adds parallel attempts or an L2CAP echo flood. Resolvable-private addresses fail to
connect (no IRK held) -> also honest `fails`; static random and public work. The passive raw-terminate family defeats
busy-refuse outright at the cost of a coprocessor board (`tools/witness` pattern).
