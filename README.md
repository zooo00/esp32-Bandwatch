# Bandwatch — Waveshare ESP32-C5-LCD-1.47

A Wi‑Fi **activity** meter and device finder for the dual‑band ESP32‑C5. Port of
[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD)'s *Bandwatch* (2.4 GHz,
ESP32‑C6) to the **Waveshare ESP32-C5-LCD-1.47**, extended with 5 GHz sweeping, Bluetooth LE scanning, IEEE 802.15.4 (Zigbee / Thread) sniffing, device tables
for all three radios, a "hunt" mode for locating one device by signal strength, a deauth attack that kicks
an AP's clients off their network, an LCD UI driven by the BOOT button, and a host‑side web dashboard with
pcap capture.

Developer documentation (architecture, serial protocol, hardware references, board quirks): [`docs/DEVELOPER.md`](docs/DEVELOPER.md).
A short orientation for AI assistants is in [`CLAUDE.md`](CLAUDE.md). Open work (bugs, verifications, features) is
tracked in [`docs/BACKLOG.md`](docs/BACKLOG.md); design write-ups are in [`docs/ROADMAP.md`](docs/ROADMAP.md); release
history is in [`CHANGELOG.md`](CHANGELOG.md).

Bandwatch listens to 802.11 traffic in promiscuous mode and reports a **busy score** (0–100) per channel as a proxy
for channel load (the separate `spec` mode reads raw 2.4 GHz RF power). It does **not** measure true airtime
occupancy. It is receive‑only with two exceptions: a running deauth attack, and BLE active scanning — `blescan active`,
or the default `auto` policy's short (4 s) active window after a new unnamed scannable device, sends BLE scan
requests. `blescan passive` keeps BLE mode fully silent.

## What it does

- **Modes**: 5 GHz (36–64, 100–144, 149–165; 25 channels, ~5.5 s per sweep), 2.4 GHz (1–13), **both**
  interleaved (38 channels, ~8.4 s), **Bluetooth LE** (continuous scan), or **802.15.4** (Zigbee / Thread,
  channels 11–26, ~3.5 s per sweep), or **spectrum** (`spec`: raw 2.4 GHz RF energy, 2400–2483 MHz at a 1/2/5 MHz
  step, flagging energy no decoded Wi‑Fi/BLE/Zigbee device explains). The C5 has one radio, so these six modes are
  time‑shared, never simultaneous.
- **Patrol**: `patrol 1` (or the dashboard's Patrol control) walks the modes on its own — spectrum 30 s, both Wi‑Fi
  bands 40 s, BLE 20 s, repeat — so the spectrum's "explained" sources stay fresh on a board left on a desk; custom
  legs with `patrol spec:20,ble:10,…` (2–6 legs, 5–600 s). No capture, hunt or deauth while it walks; picking a mode
  stops it. The LCD header shows `patrol <MODE> <s>s`.
- **Devices**: every Wi‑Fi transmitter (RSSI, max, frames, channel) and, for access points, the beacon details:
  SSID, security (Open / WEP / WPA / WPA2‑PSK / WPA2‑Enterprise / WPA3‑SAE / WPA3‑Enterprise / OWE, PMF),
  PHY generation (b/g, n, ac, ax, be), channel width, BSS‑load utilisation and client count, country. Every BLE
  advertiser: address type, name, manufacturer (Bluetooth SIG company id), Apple continuity type (AirTag / Find My,
  AirPods, Handoff, Nearby, AirPlay…), GAP appearance, service UUIDs, TX power, connectable flag. Every 802.15.4
  node: extended or PAN/short address, vendor (from the EUI‑64), protocol (Zigbee, Zigbee Green Power, Thread /
  6LoWPAN, MAC‑secured), role (beaconing coordinator/router), **permit‑join** flag, LQI.
- **Hunt**: pick one MAC (Wi‑Fi or BLE) and the LCD shows a big live RSSI with a bar, the LED colour tracks
  distance, the radio parks on the target's channel, and the dashboard plots the RSSI trend.
- **Deauth** — ⚠️ **requires a patched image; see [Known issues](#known-issues).** Pick an AP (its
  BSSID) over serial or from the dashboard, the radio parks on its channel and sends deauth frames with the AP
  spoofed as sender, so connected stations drop off and reconnect (run a capture alongside to catch WPA2
  handshakes). Stations with PMF (802.11w) enabled would ignore it by design. The attack stops itself after
  5 minutes (`kDeauthMaxMs`) so a crashed host or an unplugged cable cannot leave the board transmitting.
  Only point it at networks you are authorised to test.
  
  Two modes: 
  - `deauth <ap_bssid>` sends broadcast deauth frames to **all** clients of that AP (kick everyone).
  - `dca <client_mac> <ap_bssid>` targets a **specific client** station (disconnect just one device).
- **Per channel, every 220 ms dwell**: frames, bytes, strong frames (≥ −65 dBm), unique transmitters
  (best effort). Busy score = log‑scaled pkt/s + B/s + strong ratio + talkers, then an EMA (α 0.22).
- **LCD pages** (tap BOOT to cycle; hold BOOT to walk the modes 5g → 2.4g → both → ble → 802.15.4 → spectrum →
  boot photo, one card every 0.7 s, and release to pick the one on screen; on the Hunt page a long press stops
  the hunt). Six pages, each shown only in the modes it applies to:
  1. *Overview* (Wi‑Fi and 802.15.4): max busy score + bar, sweep count, top‑3 channels with pkt/s, per‑channel
     strip (current channel in cyan), last‑dwell stats for the current channel, transmitter estimate.
  2. *Channels* (Wi‑Fi and 802.15.4): every channel of the active band with score bar and value.
  3. *Spectrum* (spectrum mode only): peak dBm and a bar per frequency bin across 2400–2483 MHz.
  4. *Devices* (all but spectrum): the 12 strongest Wi‑Fi transmitters (SSID or MAC tail, `*` = AP), BLE devices
     or 802.15.4 nodes seen in the last 20 s, with RSSI bars.
  5. *Hunt* (while hunting): target RSSI, near/far bar, name, hits, channel. For a network-name hunt
     (`huntssid`) the name is the label, with the number of matching APs and the strongest one's channel.
  6. *System*: busiest channel details, uptime, sweeps, mode, park/capture/hunt state, radio status, free heap.
- **RGB LED** mirrors the max busy score (green → yellow → orange → red).
- **USB serial protocol** (JSON lines) for the host tool: live stats, band/park/capture commands, and raw
  802.11 frames streamed as base64 so the Mac writes standard **pcap** files.
- **BLE capture**: Bluetooth LE advertising packets record to pcap too (link type 256, BLE LL with
  pseudo-header) and dissect in Wireshark with correct PDU types, addresses and manufacturer data.
  Advertising only — reconstructed from HCI reports, so no connections, no channel number and no real CRC;
  see [`docs/DEVELOPER.md`](docs/DEVELOPER.md) §13. `blescan passive|active|auto` (default **auto**) stays passive and opens
  a short active window only when a new scannable device appears without a name — most of the naming benefit
  for a fraction of the airtime. The mode is frozen while recording so a pcap is never half passive, half
  active. The LCD and the dashboard both show which mode is in force.
- **microSD pcap recording**: the device writes the pcap itself (`sdcap 1`), so a capture does not depend on
  USB throughput. The two sinks are independent — record to card and watch live in Wireshark at the same
  time. Files can be listed and pulled back over serial without ejecting the card.
- **Event log on the card** (`events 1`, or the dashboard's *Event log* control; the setting survives a power
  cycle): an untethered record of a walk. `events.csv` gets a row for every surveillance‑OUI match and for every
  device **new to this card** — not in its `seen.csv` baseline, which grows as you walk, so early on almost
  everything is new. Randomized (privacy) addresses never count as new. Rows are timestamped once a host has sent
  the time, buffered in RAM, and written in short bursts, so the card is not held mounted. With no card in, the
  log stays armed, keeps surveillance hits, and attaches when a card appears. Extra surveillance prefixes can be
  added by putting a `surveil.csv` (`AA:BB:CC,<category>`) on the card. The dashboard pulls `events.csv` /
  `seen.csv` back without ejecting.
- **Card in / card out faces**: pulling or inserting the card shows a sad (pale blue, "SD card out", with what
  stopped) or happy (yellow, "SD card in") face on the LCD for 3 s. A recording interrupted by a pulled card is
  closed cleanly and reported, not silently cut off. There is no card‑detect pin; an idle card is probed every
  2 s, so a change shows within about 4 s. Pull the card gently: one hot‑pull in three reset the board over USB.
- **Host dashboard** (`host/bandwatch_host.py`, one page — see below): busy‑score chart per channel, load trend,
  spectrum views, **Wi‑Fi**, **Bluetooth LE** and **Zigbee / Thread** device tables (sortable, filterable, vendor
  names from the IEEE OUI registry, RSSI sparklines, a *Hunt* button per row, a *Hunt name* and a *Deauth* button per AP
  row, and a box to hunt any network name), a hunt bar with live RSSI trend (and, for a name hunt, every AP carrying
  it) and a deauth bar with a frame counter (both can show at once), mode/park/capture controls.
  Tables freeze while the mouse is over them and there is a Pause button (space bar), so buttons stay put.

## Hardware

Waveshare **ESP32-C5-LCD-1.47** (ESP32‑C5FH4, 4 MB flash, no PSRAM, 1.47" ST7789 172×320, WS2812B on GPIO8,
BOOT key on GPIO28). Pins: SCLK 7, MOSI 6, MISO 5, LCD CS 23, DC 24, RST 26, backlight 10.

## Setup, build, flash

```sh
./setup.sh              # arduino-cli + esp32 core 3.3.x (has ESP32-C5) + lvgl 9.3 + pyserial
./build.sh              # compile
./build.sh --upload     # compile + flash (auto-detects /dev/cu.usbmodem*, or PORT=/dev/cu.xxx)
```

Board settings used by `build.sh`: `ESP32C5 Dev Module`, USB CDC on boot, *Huge APP (3 MB No OTA)* partitions (`PartitionScheme=huge_app`),
4 MB flash, PSRAM disabled. LVGL uses the sketch‑local `bandwatch/lv_conf.h` via `LV_CONF_PATH`.

Notes for this board:

- **First flash / blank screen after flashing.** The C5's USB‑Serial‑JTAG can only do a *core* reset, which
  does not re‑sample the BOOT strap. If the strap was latched low (BOOT held at power‑on), the chip stays in
  ROM download mode after esptool's reset. `build.sh` detects "waiting for download" and issues a watchdog
  reset; otherwise press RESET once.
- **Never toggle DTR/RTS on the serial port** — the chip turns those edges into BOOT/EN pulses and reboots.
  The host tool and `build.sh` open the port with both lines held asserted. If you use another terminal:
  `arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=115200,dtr=on,rts=on`.
- **Apple Silicon**: Arduino's bundled `ctags` is x86_64. `build.sh` always routes ctags through
  `tools/ctags/ctags`, which wraps native Universal Ctags, so `brew install universal-ctags` is needed on
  arm64 even with Rosetta installed (`setup.sh` does it).
- **Core version is pinned** to `esp32:esp32` **3.3.11** in `setup.sh` (override with `ESP32_CORE_VERSION=…`).
  The deauth path depends on offsets inside that core's prebuilt `libnet80211.a`; see
  [`docs/DEVELOPER.md`](docs/DEVELOPER.md) §9 before bumping it.

## Host dashboard and pcap capture

```sh
python3 host/bandwatch_host.py            # http://127.0.0.1:8080 , pcaps in ./captures
python3 host/bandwatch_host.py --port /dev/cu.usbmodem21101 --http 8080 --captures ~/pcaps
```

The page polls the device state once a second. The dashboard (`dashboard2.html`, also at `/v2`) groups the
controls into a left rail — band and park, capture start/stop, hunt/deauth sharing one action bar, and the live LCD
mirror with page buttons — next to one scrolling column: stat tiles, a busy‑score bar chart per channel (grouped by
band segment, current channel marked), the channel‑load trend, the spectrum views, only the active radio's device
table open (the other two fold away as "last seen" caches), and the device log. Every file on the SD card gets a
*pull to Mac* button that streams it back over serial and offers a download. Each device table and the probe table
have a *CSV* button that downloads exactly the rows shown (filter, checkboxes and sort), with RSSI min/avg/max columns.

The classic tabbed layout (`dashboard.html`) was removed in v1.19; git tag `v1.18.2` has the last copy. `/classic`
now redirects to `/`, and `--ui` is still accepted but ignored, so old bookmarks and scripts keep working.

The dashboard is served on `127.0.0.1` by default. `/api/cmd` has no authentication and can start a deauth attack,
so the host guards it against other web pages: a command must be a `POST` with `Content-Type: application/json`, a
foreign `Origin` is refused, every request's `Host` header must name the bind address, `localhost` or `127.0.0.1`
(this blocks DNS rebinding), and pages are sent with anti‑framing headers. None of that is authentication: using
`--bind` to expose the port beyond the loopback interface (the host prints a warning) hands deauth to anyone who can
reach it. Errors come back as a non‑2xx status with `{"ok": false, "error": "..."}`; `503` means no device is
connected.

**Capture** streams every received frame (up to the snap length) over USB; the host writes
`captures/bandwatch-wifi-YYYYmmdd-HHMMSS.pcap` (`bandwatch-802154-…` in 802.15.4 mode) with a radiotap header
(TSFT, channel, dBm signal) that Wireshark opens directly. Park on one channel for a continuous capture; while hopping, each sweep contributes a 220 ms slice
per channel. Throughput is bounded by USB CDC (~300 KB/s of frame data); the device counts frames it had to drop
(`drop` in the dashboard). Frames are captured as received, i.e. encrypted payloads stay encrypted. If Wireshark
reports bad FCS on every frame, run with `--no-fcs`.

**microSD recording.** `sdcap 1` starts recording pcap directly to the card (`sdcap 0` stops); the *Record to
SD* button on the dashboard does the same. Both sinks give immediate feedback: the button latches to
"Starting…"/"Stopping…" on press rather than waiting for the next poll, a toast reports the start and then the
saved file with its frame count and size, and each sink shows how many files it has written this session. The
RGB LED **pulses while recording** — cyan for the Mac, magenta for the card, white when both are running — and
the LCD header shows `USB`, `SD` or `REC`. Capture is Wi‑Fi/802.15.4 only, so both controls are hidden in
Bluetooth LE mode. The device writes the same radiotap / 802.15.4‑TAP format the host
tool writes, so the files open in Wireshark unchanged. `sdinfo` reports card size and progress, `sdls` lists
files and `sdread <path>` streams one back over serial so captures can be retrieved without ejecting the card.

The device has no RTC: the host sends `time <epoch>` on connect, which is what dates the records and names the
files (`/bandwatch-wifi-YYYYmmdd-HHMMSS.pcap`, **UTC** — the host names its own files in local time). Without
a host the files fall back to a counter and uptime-based timestamps. FATFS costs ~30 KB of RAM, so the card is
mounted only while it is in use and released again afterwards; while recording, the capture ring is sized down
accordingly (fewer slots, so expect more `drop` on a very busy channel than with USB capture alone).

Serial commands (newline‑terminated, also usable from any terminal): `band 5g|2.4g|both|ble|154|spec`,
`park <ch>|0`, `cap 0|1`, `snap <bytes>`, `specstep 1|2|5`, `blescan passive|active|auto`, `addr1 0|1`,
`hunt <mac|ext-addr|pan/short> [ch]` / `hunt 0`, `huntssid <name>` / `huntssid 0` (hunt a network name: every AP
beaconing it, exact and case-sensitive; the device parks on the strongest), `deauth <bssid>` / `deauth 0` (Wi‑Fi modes only — broadcast
deauth to all clients of that AP, auto‑stops after 5 min), `dca <client_mac> <ap_bssid>` / `dca 0` (targeted deauth
to one specific station), `sdcap 0|1`, `sdinfo`, `sdls`, `sdread <path>`, `sdrm <name>`, `events 0|1`,
`alerts 0|1`, `ledtest surv|new|join`, `mirror 0|1`, `page next|prev`, `patrol 1|0` / `patrol <mode>:<sec>,…`,
`time <epoch>`, `info`, `reboot`, and the
diagnostics `sdprobe` and `sdface 0|1`. Each is described in [`docs/DEVELOPER.md`](docs/DEVELOPER.md) §4.
Changing mode (`band …`, or holding BOOT) always ends a capture and frees the capture ring, so restart it with
`cap 1` afterwards.
802.15.4 captures use the 802.15.4‑TAP pcap link type (Wireshark decodes Zigbee/Thread; encrypted payloads need
the network key, Wireshark knows the default Zigbee trust‑centre key).

Vendor names: the host downloads the IEEE OUI registry (`oui.csv`, ~3 MB) once into `~/.cache/bandwatch/` and
uses a small built‑in table until then (`--no-oui-download` to skip). Randomized MACs (most phones, BLE random
addresses) have no vendor by design.

## Moving to another machine

Everything needed is in this repository. Clone it, run `./setup.sh`, then `./build.sh --upload`. The only
per‑machine state is the Arduino core and libraries that `setup.sh` installs.

## Tuning knobs (`bandwatch/bandwatch_core.h`, SD in `sd_sink.cpp`)

`kChannels[]`, `kDwellMs` (220), `kStrongThresholdDbm` (−65), `kBusyEmaAlpha` (0.22), `kLongPressMs` (700),
`kCapSlotsMax/Min` / `kCapMaxLen` / `kCapSmallLen` (capture ring: up to 20 slots of 1600 B in Wi‑Fi, 128 B in
BLE/802.15.4, sized down while the card is mounted),
`kCountryCode` ("EU", only affects the regulatory table),
`kDeauthMaxMs` (5 min, the deauth dead‑man's switch), `kSdCsPin` (4), `kSdSpiHz` (20 MHz), `kSdBufSize` (4 KB,
matches the FATFS sector size), `kSdFlushMs` (5 s), `kSdBudgetUs` (8 ms of SD writing per loop).

## Known issues

**The deauth attack needs a patched image** (resolved in 1.6; it did not work at all before that).
`./build.sh --upload` applies the patch automatically and restores the stock `.bin` afterwards, so the build
directory is never left in a patched state. `./build.sh --upload --no-patch` flashes the stock image, whose
deauth fails loudly with `ESP_ERR_INVALID_ARG` and transmits nothing.

```
./build.sh --upload                    # patched
python3 tools/witness/verify.py        # confirm from the air
```

`esp_wifi_80211_tx()` refuses deauthentication frames: the prebuilt `libnet80211.a` calls
`ieee80211_raw_frame_sanity_check()` first and bails if it returns nonzero. Espressif document the API as
supporting *"beacon/probe request/probe response/action and non-QoS data"* only. The patcher overwrites that
check's prologue with `return 0` in the linked image and reseals the checksum and SHA-256. `-Wl,--wrap` does
**not** work — the check and its caller share an object file — and an ESP-IDF rewrite would not help, since the
blob is identical there. See [`docs/DEVELOPER.md`](docs/DEVELOPER.md) §11, and
[`docs/WHY-DEAUTH-WAS-HARD.md`](docs/WHY-DEAUTH-WAS-HARD.md) for why this took seven releases to find.

This is rule-8 fragility squared: the patch is pinned to one exact core build, the address moves whenever the
sketch changes (it is resolved from the ELF every run, never hardcoded), and disabling the check lets malformed
frames through generally. Re-verify with the witness after any toolchain change.

**The counters do not prove the attack works.** `deauthSent`/`da` is incremented unconditionally and
`ic_tx_pkt()` returns `void`; `da` once reached 328 in a window where an external witness heard exactly zero
frames. Only a second radio in monitor mode can tell you — that is what `tools/witness/` is for.

**The driver-internal slot path (`kickpath 1`) is dead.** It transmits nothing for *any* frame subtype, not
just deauth, which means the reverse-engineered offsets in `docs/DEVELOPER.md` §9 are wrong rather than merely
fragile. It is kept only as a starting point for fixing them.

**Real stations do disconnect** - confirmed by the owner on their own network (2026-10-08, v1.19.x): stations drop. Correct deauth frames demonstrably reach the air (witness,
§11 in DEVELOPER). PMF-enabled networks ignore these frames by design.

## Limitations

- One radio: Wi‑Fi bands, Bluetooth LE and 802.15.4 are exclusive modes. No Bluetooth Classic on the C5. The
  prebuilt BLE stack scans legacy advertisements only (Bluetooth 5 extended / coded‑PHY advertising is compiled
  out of the Arduino core). 802.15.4 protocol detection is heuristic (network-layer header bytes); Zigbee/Thread
  payloads are encrypted on the air and stay encrypted here.
- RAM: 320 KB with no PSRAM. Only the visible LCD page exists as widgets; the capture ring is allocated per
  capture; BLE keeps no result cache of its own, so BLE mode idles at ~88 kB free (measured in v1.15.2).
- The busy score is a traffic proxy, not calibrated airtime; thresholds were tuned on 2.4 GHz, so a busy
  802.11ac/ax channel may read a little high.
- DFS channels are received passively; any channel the driver refuses is skipped and marked `x`.
- Not a replacement for professional RF tools.

## Credits

Bandwatch began as a port of **[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD)**
— the original *Bandwatch* 2.4 GHz Wi‑Fi activity meter for the Waveshare **ESP32‑C6**‑LCD‑1.47. Full credit to
[Pierre Gode](https://github.com/PierreGode) for that base: the core idea, the LCD/LVGL display glue
(`Display_ST7789.*`, `LVGL_Driver.*`, `lv_conf.h`, pins since changed), and the first host dashboard and sweep logic.

This project **adapted it to the Waveshare ESP32‑C5‑LCD‑1.47** (ESP32‑C5, with the pin map, board quirks and
single-radio constraints that board brings) and then **kept building on it** — adding dual‑band 5 GHz sweeping,
Bluetooth LE scanning, IEEE 802.15.4 (Zigbee / Thread) sniffing, a 2.4 GHz energy‑detect spectrum mode, per‑radio
device tables, a signal‑strength "hunt" locator, microSD pcap recording, surveillance‑OUI flagging, the deauth
investigation, and a rebuilt host dashboard. See [`CHANGELOG.md`](CHANGELOG.md) for the full trail.

The BLE direct‑NimBLE approach was studied from
[shermanatoor/ouispy‑blesniff](https://github.com/shermanatoor/ouispy-blesniff).

## License

This project's own work is released under the **[MIT License](LICENSE)**.

That MIT grant covers **only this project's contributions** — the ESP32‑C5 port and everything built on top of
the upstream base. Bandwatch is a derivative of
[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD), which publishes **no license
of its own**; those upstream‑derived portions remain © Pierre Gode, all rights reserved, and are **not** relicensed
here. If you want to reuse the upstream‑derived parts, ask the original author. See [`NOTICE`](NOTICE) for the full
breakdown. The `bootlogo/` boot images are third‑party artwork and are likewise not covered.

## Versions

Release history, newest first, is in [`CHANGELOG.md`](CHANGELOG.md). The running firmware reports its version in
`hello.ver`, which the dashboard shows.
