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
| microSD | SPI, CS **GPIO4** (shares SCLK/MOSI with the LCD; unused so far) | |

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
- Reference build (core 3.3.11, lvgl 9.3.0, **v1.6**): 0 errors, 0 warnings, **1 880 889 B flash (59 % of the
  3 MB app partition)** and **79 592 B static RAM (24 %)** — that static figure is ~400 B under the ~80 KB
  ceiling in `CLAUDE.md` rule 4, so watch it when adding globals, and see §17 for how `apSuffix` was added
  without moving it at all.
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
  25 × 5 GHz, 16 × 802.15.4), `BandMode` enum (`5g`, `2.4g`, `both`, `ble`, `154`), capture ring limits, LCD
  page enum; shared structs (`Accum`, `ChannelState`, `CapFrame`, `Hunt`, `Deauth`, `BleState`, `SdSink`,
  `DevSnap`) and state externs; `serialRoom()` / `sanitizeText()` / `putLE16/32`; cross-module decls.
- `bandwatch.cpp` — the core: channel control (`applyChannelIdx()`, `advanceChannel()` skipping
  disabled/rejected channels, honouring `parkedIdx`; `setBandMode()` is the only place radios are swapped),
  dwell scoring (`computeBusyScore()` log-scaled pkt/s, B/s, strong ratio, unique talkers; smaller references in
  802.15.4 mode; EMA α = 0.22, `globalActivityMax()`, `sortTop3()`), device-table snapshots (copy under lock,
  sort outside), hunt orchestration (`startHunt/stopHunt/lookupHuntLabel`), and the entry points —
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
- `sd_sink.cpp` — microSD pcap sink: on-demand FATFS mount (§12), byte-compatible writer, `sdls` / `sdread`;
  since 1.18 also card presence (`sdSetPresent()`, the one place it changes; the idle CMD0 probe
  `sdServicePresence()`) and the one I/O-failure path for a card pulled mid-capture (§12).
- `events.cpp` — the C4 event log (1.18, §20): `eventFlag()` (radio side, under `g_devMux`), `serviceEvents()`
  (loop: classify, buffer, flush to `/events.csv`), the `/seen.csv` novelty baseline, and `loadSurvExtra()`
  (`/surveil.csv` extra OUIs, read at boot).
- `settings.cpp` — NVS persistence (C3, 1.11; the `events` flag joined in 1.18).
- `host_proto.cpp` — the serial JSON protocol: `sendHello/sendDwell/sendSweep/sendBleStatus/sendDevices`,
  `handleCommand()` / `pollSerial()`. All output goes through `serialRoom()` so a line is either written whole
  or skipped.
- `lcd_ui.cpp` — LVGL 9, 172×320 portrait: `showPage()` deletes the current page's widgets and builds the new
  one (`buildOverviewPage`, `buildChannelsPage`, `buildDevicesPage`, `buildHuntPage`, `buildSystemPage`);
  `refreshUi()` updates only the visible page and drives the LED. `pollButton()` = tap → next page, hold 0.7 s →
  next mode (or stop hunt on the Hunt page).
- `devices.h` — device-table structs + open-addressing hash; `surv_ouis.h` — the surveillance-OUI table
  (matched in firmware so the LCD can flag without a host); `Display_ST7789.*`, `LVGL_Driver.*`, `lv_conf.h`
  — display glue.

Tasks and contexts: Arduino `loop` task (LVGL + everything in `Bandwatch_Loop`), Wi‑Fi task (promiscuous
callback), NimBLE host task (advert callback), 802.15.4 ISR. Shared data is protected with the two spinlocks;
never call anything that allocates or blocks inside them.

Memory budget (measured, v1.2): static ≈ 79 KB; free heap ≈ 100 KB in Wi‑Fi modes, 60–100 KB in BLE mode,
≈ 120 KB in 802.15.4 mode; capture ring = up to 20 × 1.6 KB allocated on `cap 1`.

## 4. Serial protocol (USB CDC, 115200, newline-delimited)

Device → host, one JSON object per line unless noted:

| Line | When | Fields |
| --- | --- | --- |
| `{"t":"hello",...}` | boot, `info`, after `band` | `fw`, `ver`, `dwell_ms`, `band` (mode), `country/bandmode/proto/promisc` (esp_err names), `chs` (channel list of the mode), `park`, `cap`, `snap`, `heap`, `up` (s), `rst` (reset reason), `hunt` (id or null), `h` (hunt status `[rssi, age_ms, hits]` or null), `deauth` (`[bssid, park ch (0 if hopping), frames sent, frames failed]` or null), `sd` (`{mounted, mb, cap, file, frames, bytes, err, clock}` — `mounted` is historical naming: it reports `sd.cardPresent`, *a card is in the slot*, not that FATFS is mounted; since 1.18 it follows removal/insertion, §12), `mir` (§19), `ev` (event-log status, 1.18 — see the `ev` row) |
| `{"t":"d",...}` | every completed dwell | `c` channel, `s` EMA score, `r` raw score, `f` frames, `b` bytes, `st` strong, `u` unique, `g` global max, `n` sweep no., `park`, `cap`, `drop` (capture drops), `da` (deauth frames sent so far; 0 when idle), `df` (deauth frames failed so far; 0 when idle), `sdc` (1 while recording to microSD), `sdf`/`sdb` (frames/bytes written to the card), `h` |
| `{"t":"s",...}` | after every full sweep | `n`, `g`, `band`, `ch`: `[[ch, ema, frames, bytes, strong, unique, state], ...]` (state 0 ok / 1 no data / 2 rejected), `aps`, `drop`, `heap` |
| `{"t":"w","dev":[...]}` | every 2 s in Wi‑Fi modes, **in chunks of ≤24 rows** (v1.17; loudest first, each chunk a complete line; the host merges by MAC, so a chunk dropped for TX room just waits a cycle) | rows `[mac, rssi, max, frames, age_ms, ch, flags, ssid, sec, pmf, phy, bw, util, stations, cc, surv, apSuffix]`; `apSuffix` = last 3 bytes of the BSSID this device was heard associated with, lower-case hex, `""` if never seen on a BSS (§17); flags bit0 AP, bit1 IEs parsed, **bit2 seen only as a frame destination (tier 1)**; `surv` = surveillance category id (0 none); `sec` bits: 0x01 WEP, 0x02 WPA, 0x04 WPA2‑PSK, 0x08 WPA2‑Ent, 0x10 WPA3‑SAE, 0x20 WPA3‑Ent, 0x40 OWE, 0x80 open; `pmf` 0/1/2; `phy` bits 1 legacy, 2 n, 4 ac, 8 ax, 16 be; `bw` in 10 MHz units; `util` 0–255; `cc` country |
| `{"t":"b","dev":[...]}` | every 2 s in BLE mode | rows `[mac, rssi, max, adverts, age_ms, addrType, company, name, appearance, txPower(127=none), svcUuid16, svcDataUuid16, appleType, flags, surv]`; flags bit0 connectable, bit1 legacy adv, bit2 scannable |
| `{"t":"z","dev":[...]}` | every 2 s in 802.15.4 mode | rows `[id, rssi, max, frames, age_ms, ch, pan, short, proto, flags, lqi]`; `id` = extended address `aa:bb:cc:dd:ee:ff:00:11` or `pan/short` hex; `proto` 0 unknown, 1 Zigbee, 2 Zigbee GP, 3 Thread/6LoWPAN, 4 MAC‑secured; flags bit0 ext addr, bit1 beacons, bit2 permit join, bit3 MAC security, bit4 data seen |
| `{"t":"pr",...}` | a directed probe request (Wi‑Fi modes, C1) | `mac` (the probing client; often a randomized, locally administered MAC), `rssi`, `ch`, `ssid` (the network it asked for, control-stripped). Wildcard probes (empty SSID) are not sent. The device suppresses a repeat of the same (MAC, SSID) pair for 60 s (32-entry table in `host_proto.cpp`); the host keeps history and expires pairs after 15 min (`PROBE_TTL_S`), grouping by SSID because phones randomize per burst |
| `{"t":"ble",...}` | every 1 s in BLE mode | `devs`, `cycles`, `heap`, `adv` (advertising reports), `scan` (policy) / `running` (what is actually running) / `switches`, `cap`, `drop`, `sdc`/`sdf`/`sdb`, `h`. **BLE mode emits no dwell lines, so this is the only live capture telemetry there** - anything added to `{"t":"d"}` for the dashboard has to be added here too |
| `{"t":"ev","ev":{...}}` | every 5 s while the event log is armed, in every mode (`sendEventStatus()`, 1.18) | `ev` = `{on, card, base, file, written, pending, surv, new, drop, err, wait}`: armed; card usable (last attach worked); baseline hashes in RAM; entries in `/seen.csv` (counted at load, plus this session's appends); rows written to `/events.csv`; rows buffered; surveillance rows; new-device rows; rows dropped (2 KB buffer full, rows discarded by `events 0` on a busy card, or the 16-entry radio queue full); card errors (failed mount/write); novelty checks skipped for want of a baseline. The same object rides on `hello` and the `events` ack. §20 |
| `{"t":"ack",...}` / `{"t":"log","msg"}` / `{"t":"err","msg"}` | command replies and notices | Since 1.18: `{"t":"log","msg":"sd card removed[: why]"}` / `"sd card inserted"` on every presence change after boot (`why` e.g. `recording stopped`, `file pull stopped`, `event log buffering`) — the host sets `sd.mounted` from it and drops its stale file list; `{"t":"err","msg":"sdcap: write failed after N frames (card removed?) - recording stopped"}` (host clears `cap`); `{"t":"err","msg":"sdread: read failed at X of Y bytes (card removed?)"}` *instead of* `sdread_done` when a pull comes up short, so the host discards the partial file rather than saving it as complete |
| `S <n> <base64>` | after `sdread <path>` | one chunk of a file being streamed off the card; bracketed by `sdread` / `sdread_done` acks |
| `{"t":"sdls","files":[[name, bytes], ...],"total":N,"sent":M}` | after `sdls` | files in the card root; `sent < total` = the serial buffer filled mid-list (host slow or absent) and whole entries were dropped |
| `P <ch> <rssi> <ts_us> <len> <base64>` | while `cap 1` | one captured frame; `len` = original length, payload may be truncated to the snap length. Wi‑Fi frames include the FCS; 802.15.4 frames exclude it |

Host → device commands: `band 5g|2.4g|both|ble|154`, `park <ch>` / `park 0`, `cap 1|0`, `snap <32..1600>`,
`hunt <mac> [ch]` / `hunt <ext addr>` / `hunt <pan>/<short>` / `hunt 0`, 
`deauth <bssid>` (Wi‑Fi modes only — broadcast deauth to all clients of that AP; stops itself after `kDeauthMaxMs`,
5 min) / `deauth 0`, 
`dca <client_mac> <ap_bssid>` (targeted deauth to one specific station — both MACs colon-separated, the
device rejects anything else) / `dca 0`, 
`sdcap 0|1`, `sdinfo` (mounts, replies, and since 1.18 unmounts again), `sdls`, `sdread <path>`, `time <epoch>`, `info`, `reboot`.
1.18: `events 1|0` (arm/disarm the SD event log, §20; persisted in NVS; arming works with no card; ack
`{"t":"ack","cmd":"events","ev":{...}}`, or `{"t":"err","msg":"events: not enough free heap"}` when its ~12.6 KB
will not allocate), `sdprobe` (diagnostic: `{"t":"ack","cmd":"sdprobe","r1":N,"present":0|1}` — the raw CMD0
reply of the presence probe; `r1` is -1 when the card is mounted or busy and the probe was not sent),
`sdface 0|1` (diagnostic: show the card-out / card-in LCD face for 3 s without touching the card).

The device drops a whole line rather than truncating it, so the host must tolerate missing lines — but it must
also tolerate *malformed* ones: `handle_line()` wraps the dispatch so a short or unexpected line is logged
instead of killing the reader thread (an exception there closes the serial port and silently ends a capture).

## 5. Host tool (`host/`)

`bandwatch_host.py`: a reader thread parses lines into a state dict (channels, history, device tables with
per-device RSSI history, hunt state), an HTTP server exposes `GET /api/state` (everything, JSON) and
`POST /api/cmd` (`{"cmd":"band"|"park"|"capture"|"hunt"|"deauth"|"dca"|"info", ...}`), and `PcapWriter` writes radiotap pcaps
for Wi‑Fi and 802.15.4‑TAP pcaps for 802.15.4. Files are named `bandwatch-wifi-YYYYmmdd-HHMMSS.pcap` /
`bandwatch-802154-…` in `--captures` (default `./captures`).

`captures/` is **git-ignored** — it holds your recorded pcaps, not source, so it is the one thing a fresh
`git clone` on another machine does not reproduce. Everything else regenerates (`build/` from `./build.sh`,
the toolchain from `./setup.sh`, `~/.cache/bandwatch/oui.csv` on first host run); the recordings do not. Copy
them by hand if you want them on the other machine.

The deauth command starts a broadcast deauth attack (`cmd: "deauth", bssid: "XX:XX..."`), while the dca command 
starts a targeted attack on one client (`cmd: "dca", client_mac: "...", ap_bssid: "..."`).

`/api/cmd` has **no authentication**, and one of its commands starts a deauth attack, so the server binds to
`127.0.0.1` by default; `--bind 0.0.0.0` hands that to anyone who can reach the port. Values that reach the
serial line have embedded CR/LF stripped in `Bandwatch.send()` so a crafted field cannot append a second
command to the line. The pcap writer is touched from both the reader thread and the HTTP thread, so
`handle_frame()` takes a local reference and tolerates the file being closed underneath it.

Vendor names: IEEE OUI CSV cached in `~/.cache/bandwatch/oui.csv`
(downloaded once in the background) with a built-in fallback; Bluetooth company ids, Apple continuity types,
GAP appearance categories and common service UUIDs are small tables at the top of the file.

Two dashboards, both always served: `dashboard2.html` (the default, at `/` and `/v2`) and the classic
`dashboard.html` (at `/classic`). `--ui classic` swaps which one sits at `/`; the other paths keep resolving,
and each page links to the other by absolute path. `host/run-v2.sh` is a leftover alias for the default.
`dashboard2.html`: same polling and data, controls in a sticky left rail (band/park, capture, hunt/deauth action
bar, LCD mirror with page buttons, SD card list with pull-to-Mac), one scrolling main column, and only the active
radio's device table open (the other two fold into "last seen" caches). Newer features (LCD mirror, §19) exist
only here.

`dashboard.html` (classic): no framework, polls `/api/state` once a second. Tabs: Overview (bar chart, trend, channel
table), Wi‑Fi devices, Bluetooth LE, Zigbee/Thread; a hunt panel appears when a hunt is active and a red deauth
card with the frame counter while an attack runs. Tables are
rendered keyed by device id so rows keep identity; while the mouse is over a table (or the page is paused with
the button / space bar) rows neither move nor re-render, so buttons stay put. Colours follow a light/dark token
set; charts are inline SVG.

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
installs the newest `esp32:esp32`, so pin the version there if you need a reproducible build.

Guards that are in place: a null `desc` and a null ebuf pointer `P` both count a TX failure and return rather
than dereferencing; `serviceDeauth()` stops the attack after `kDeauthMaxMs` so a dead host or unplugged USB
cable cannot leave it transmitting; `stopDeauth()` restores the 8.2 dBm sniffing TX power that `startDeauth()`
raised to 16 dBm. Failures are visible as `df` in `d` lines and the 4th element of `deauth` in `hello`.

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
  (`sanitizeText`) so `printJsonStr` cannot expand them into `\u00xx` escapes that overshoot the `serialRoom()`
  budget for a line. The budgets (`40 + n*126` Wi‑Fi, `40 + n*102` BLE) are estimates, not exact lengths: a full
  64-device table already needs ~7 KB of the 8 KB TX buffer, so raising them is not free.

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

Whether a real station actually disconnects. Everything above establishes that correct deauth frames reach the
air; it does not establish that any particular client honours them. That needs a device you own, associated to
an AP you own. Unprotected-frame handling varies by supplicant, and PMF-enabled networks will ignore these
frames by design.


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
`sdcap`/`sdinfo`/`sdls`/`sdread`, and released by `sdUnmount()` when done. Measured on hardware:

| state | free heap |
| --- | --- |
| idle, card present but unmounted | 83.4 kB |
| recording to SD | 31.9 kB |
| after stopping | 83.4 kB |
| BLE mode (floor is 28 kB) | 57.8 kB |

`sdcap` opens the file **before** sizing the capture ring, so `kCapHeapReserve` is reserved against
post-mount heap. The ring therefore gets fewer slots while recording to SD (8 rather than 20) — expect a
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
mode now idles at ~95 kB free instead of ~58 kB, and the device table sees slightly more devices.

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
**~83 kB**, because the Wi-Fi/BLE driver stacks and the FreeRTOS task stacks claim the rest once the radio
comes up. Reading the linker figure literally suggests roughly 3× more room than exists, which is a good way
to talk yourself into a change that then crashes in the field.

### What actually binds: free heap at peak concurrent load

Measured on hardware (read `heap` from `hello`, or the dashboard):

| state | free heap |
| --- | --- |
| idle, Wi-Fi | ~83 kB |
| idle, BLE | ~95 kB |
| SD capture — the tightest normal state | **31.9 kB** |
| BLE + SD capture | 32.3 kB |
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
4 B = 10,240 B), the CSV row buffer (2,048 B) and the pending `/seen.csv` appends (48 × 6 = 288 B) — about
12.6 KB, malloc'ed by `events 1` (refused with an error if it will not fit) and freed by `events 0`. It mounts
FATFS only for the length of a flush and never alongside a capture (flushes wait while `sdcap` records), so the
FATFS cost does not stack on the SD-capture worst case above — but the 12.6 KB are gone from *every* state while
armed, BLE and capture included, and arming is persisted. Free heap at peak load with the log armed has **not**
been measured. Static cost of the 1.18 work (event log + presence probe + faces): 76,816 → **77,272 B** (the
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
headroom above), so rule 4 still keeps the rest honest: current static usage is 77,272 B (v1.18; 74,032 B at v1.15.4), under 3 kB
below the line, and that headroom is the edge of an unverified budget rather than a wall.

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
sample count). Only 16 channels sweep here, so the longer rows stay well inside the `serialRoom(1500)` budget;
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
from `edMax` via `edDbmToScore()`, available only in `spec` mode (`pageAvailable`). Dashboard: a Spectrum tab
with a dBm bar chart, a client-side waterfall (`<canvas>`, one row per completed sweep), and the
unexplained-energy list from `s.unidentified`.

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

### Pacing: why a naïve full-frame drops
All dirty regions in one `lv_timer_handler()` flush **synchronously** back-to-back, and `Serial` is
non-blocking with an 8 KB TX buffer (`setTxTimeoutMs(0)`, §6) — it never drains mid-handler. A bulk repaint
(page switch, mode splash, the first frame after enabling) is ~21 chunks of ~7 KB each; only the first fits,
the rest are dropped whole (never truncated, §6). Re-invalidating the whole screen would just re-send the top
and livelock.

So `serviceMirror()` (`lcd_ui.cpp`, called once per `Bandwatch_Loop`) re-sends the screen **one ~10-row strip
per loop** via `lv_obj_invalidate_area()` — each strip is ~4.6 KB base64, well under the TX buffer, and the
buffer drains between loops, so a full frame converges over ~40 loops (<1 s). Steady-state label updates are
tiny and flush immediately. A strip can still be dropped when it collides with a data-refresh flush in the
same `lv_timer_handler` (their combined bytes exceed the TX buffer); `mirrorOnFlush` then calls
`mirrorNoteDrop()`, which sets a flag so `serviceMirror()` runs **another full pass after the current one**,
repeating until a pass completes with no drop. Restarting the scan on every drop instead would livelock on a
busy screen, and ignoring mid-pass drops (the 1.13 bug) left permanent black/stale bands where a strip was
lost in the single initial scan. Enabling the mirror (`mirror 1`) calls `mirrorRequestFull()` for the first
frame.

### Host + dashboard
`handle_mirror()` (`bandwatch_host.py`) parses the `M` line and blits the region into a 172×320 RGB565
framebuffer under `screen_lock`, bumping `screen_seq`. `GET /screen.bin` serves the raw bytes (with
`X-Screen-W/H/Seq` headers); `GET /api/screen` returns `{on, seq, w, h}` (`on` = an `M` line within 3 s). The
**v2 dashboard** (`dashboard2.html`) has a *Start mirror* card: the toggle POSTs `mirror 1|0`, a 4 Hz poll
watches `seq`, and on a change it fetches `/screen.bin` and paints it onto a `<canvas>` (RGB565 LE → RGBA,
`image-rendering: pixelated`). `sendHello` carries `"mir"` so the button reflects the device state on load.
The classic dashboard does not have the mirror card.

The mirror panel lives in the v2 **left control rail** (the 172×320 canvas fits its width). Alongside it are
**Page ‹ ›** buttons that POST `page prev|next`, which the device handles with `stepPage()` (`lcd_ui.cpp`) —
the same action as a BOOT tap (`showPage(currentPage ± 1)`, skipping pages that don't apply to the mode, and
clearing any mode splash). This lets you drive the LCD from the browser and hold on a page. It matters because
the serial link can't keep up with a continuously-repainting page: in `spec` mode the bars redraw several
times a second, so more pixels change per second than fit through the TX buffer and the mirror tears / re-scans
continuously. Stepping to a mostly-static page (Devices, System, a parked channel) lets it settle to a clean
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

### Novelty baseline
On attach, `loadBaseline()` reads `/seen.csv` and keeps the **newest 2048 entries** (a ring over the file) as
sorted 32-bit FNV-1a hashes for binary search; `kBaseCap` = 2560 leaves room for 512 new devices this session
(past that a MAC is still logged as new but not remembered in RAM, so a re-created slot could log it again). New
MACs are appended to `/seen.csv` on each flush, so the baseline grows with every walk and a device counts as new
once per card. The file itself is never trimmed. A 32-bit hash can collide; a collision makes a new device look
known (a missed row), never the reverse.

### Buffering and flushing
Rows go into a 2 KB heap buffer (~25-30 rows); overflow is counted in `drop`, not hidden. The card is **not**
kept mounted (rule 11): it is mounted just long enough to append, when 16 rows are waiting, 48 new MACs are pending
for `/seen.csv`, or 60 s have passed with anything pending. `/events.csv` rotates at 1 MB to `/events.old.csv`
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
ack, and a `{"t":"ev"}` line every 5 s while armed, in every mode. Both dashboards show it with an arm/disarm
control and pull buttons for `events.csv` / `seen.csv`; the host's `sdread` guard accepts the card's text files
(`CARD_TEXT_FILES`: `events.csv`, `events.old.csv`, `seen.csv`, `surveil.csv`) besides the pcap naming scheme.

### `/surveil.csv` (1.6.4)
Up to 64 extra OUIs, one `AA:BB:CC,<category 1-7>` per line (categories as `kSurvName` in `surv_ouis.h`),
read once at boot by `sdProbeAtBoot()` → `loadSurvExtra()` into heap (absent file = nothing allocated).
`survLookup()` checks the built-in table first. A changed file takes effect on the next boot. Same honesty rule as
§15: a match is evidence, not identification.

### Memory
Nothing while off. Armed: ~12.6 KB heap (baseline 10,240 B + rows 2,048 B + pending appends 288 B), plus FATFS
(~30 KB) only for the duration of a flush. Static: the queue and counters (§16).

### Measured on hardware (1.18 session)
First flush at 16 rows; correct CSV with epoch timestamps; `seen.csv` at 31 entries after about 80 s. Card pulled
mid-`sdcap` and re-inserted: no rows lost (§12). The re-insert of pending new MACs into the reloaded baseline was
added *after* that test, because `base` read 32 instead of about 52 — it has not been re-run since.

### Open
- **LED blip on a new/surveillance event: undecided**, deliberately coupled with C10 (permit-join blip) and
  1.6.1's transient-alert proposal so `driveLed()` gets one decision, not three.
- The presence probe has no open items (card out reads `0xFF`, card in `0x01`, removal detected within ~4 s;
  §12). Hot-pulling the card can intermittently reset the board over USB (`rst: usb`, a hardware effect, §12).
- Not yet measured: free heap at peak load with the log armed; behaviour with a large (> 2048-entry) `/seen.csv`.
