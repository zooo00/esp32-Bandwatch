# Bandwatch — Waveshare ESP32-C5-LCD-1.47

A Wi‑Fi **activity** meter and device finder for the dual‑band ESP32‑C5. Port of
[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD)'s *Bandwatch* (2.4 GHz,
ESP32‑C6) to the **Waveshare ESP32-C5-LCD-1.47**, extended with 5 GHz sweeping, Bluetooth LE scanning, IEEE 802.15.4 (Zigbee / Thread) sniffing, device tables
for all three radios, a "hunt" mode for locating one device by signal strength, a deauth attack that kicks
an AP's clients off their network, an LCD UI driven by the BOOT button, and a host‑side web dashboard with
pcap capture.

Developer documentation (architecture, serial protocol, hardware references, board quirks): [`docs/DEVELOPER.md`](docs/DEVELOPER.md).
A short orientation for AI assistants is in [`CLAUDE.md`](CLAUDE.md).

Bandwatch listens to 802.11 traffic in promiscuous mode and reports a **busy score** (0–100) per channel as a proxy
for channel load. It does **not** measure RF power, true airtime occupancy, or non‑Wi‑Fi interference, and — unless
a deauth attack is running — it never transmits.

## What it does

- **Modes**: 5 GHz (36–64, 100–144, 149–165; 25 channels, ~5.5 s per sweep), 2.4 GHz (1–13), **both**
  interleaved (38 channels, ~8.4 s), **Bluetooth LE** (continuous scan), or **802.15.4** (Zigbee / Thread,
  channels 11–26, ~3.5 s per sweep). The C5 has one radio, so these are time‑shared, never simultaneous.
- **Devices**: every Wi‑Fi transmitter (RSSI, max, frames, channel) and, for access points, the beacon details:
  SSID, security (Open / WEP / WPA / WPA2‑PSK / WPA2‑Enterprise / WPA3‑SAE / WPA3‑Enterprise / OWE, PMF),
  PHY generation (b/g, n, ac, ax, be), channel width, BSS‑load utilisation and client count, country. Every BLE
  advertiser: address type, name, manufacturer (Bluetooth SIG company id), Apple continuity type (AirTag / Find My,
  AirPods, Handoff, Nearby, AirPlay…), GAP appearance, service UUIDs, TX power, connectable flag. Every 802.15.4
  node: extended or PAN/short address, vendor (from the EUI‑64), protocol (Zigbee, Zigbee Green Power, Thread /
  6LoWPAN, MAC‑secured), role (beaconing coordinator/router), **permit‑join** flag, LQI.
- **Hunt**: pick one MAC (Wi‑Fi or BLE) and the LCD shows a big live RSSI with a bar, the LED colour tracks
  distance, the radio parks on the target's channel, and the dashboard plots the RSSI trend.
- **Deauth** — ⚠️ **does not currently work; see [Known issues](#known-issues).** The intent: pick an AP (its
  BSSID) over serial or from the dashboard, the radio parks on its channel and sends deauth frames with the AP
  spoofed as sender, so connected stations drop off and reconnect (run a capture alongside to catch WPA2
  handshakes). Stations with PMF (802.11w) enabled would ignore it by design. The attack stops itself after
  5 minutes (`kDeauthMaxMs`) so a crashed host or an unplugged cable cannot leave the board transmitting.
  Only point it at networks you are authorised to test.
- **Per channel, every 220 ms dwell**: frames, bytes, strong frames (≥ −65 dBm), unique transmitters
  (best effort). Busy score = log‑scaled pkt/s + B/s + strong ratio + talkers, then an EMA (α 0.22).
- **LCD pages** (tap BOOT to cycle, hold BOOT ≈0.7 s to cycle mode 5g → 2.4g → both → ble → 802.15.4; on the
  Hunt page a long press stops the hunt):
  1. *Overview* (Wi‑Fi modes): max busy score + bar, sweep count, top‑3 channels with pkt/s, per‑channel spectrum
     strip (current channel in cyan), last‑dwell stats for the current channel, transmitter estimate.
  2. *Channels* (Wi‑Fi modes): every channel of the active band with score bar and value.
  3. *Devices*: the 12 strongest Wi‑Fi transmitters (SSID or MAC tail, `*` = AP) or BLE devices seen in the last
     20 s, with RSSI bars.
  4. *Hunt* (while hunting): target RSSI, near/far bar, name, hits, channel.
  5. *System*: busiest channel details, uptime, sweeps, mode, park/capture/hunt state, radio status, free heap.
- **RGB LED** mirrors the max busy score (green → yellow → orange → red).
- **USB serial protocol** (JSON lines) for the host tool: live stats, band/park/capture commands, and raw
  802.11 frames streamed as base64 so the Mac writes standard **pcap** files.
- **microSD pcap recording**: the device writes the pcap itself (`sdcap 1`), so a capture does not depend on
  USB throughput. The two sinks are independent — record to card and watch live in Wireshark at the same
  time. Files can be listed and pulled back over serial without ejecting the card.
- **Host dashboard** (`host/bandwatch_host.py`): Overview tab (bar chart per channel, trend, table), **Wi‑Fi devices**
  **Bluetooth LE** and **Zigbee / Thread** tabs (sortable, filterable, vendor names from the IEEE OUI registry,
  RSSI sparklines, a *Hunt* button per row and a *Deauth* button per AP row), a hunt panel with live RSSI trend,
  a red deauth card with a frame counter while an attack runs, mode/park/capture controls.
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

Board settings used by `build.sh`: `ESP32C5 Dev Module`, USB CDC on boot, *No OTA (2 MB app)* partitions,
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

The page polls the device state once a second: band and park controls, capture start/stop, stat tiles, a busy‑score
bar chart per channel (grouped by band segment, current channel marked), a 10‑minute trend of the max score, a
per‑channel table and the device log.

The dashboard is served on `127.0.0.1` only. `/api/cmd` has no authentication and can start a deauth attack,
so think before using `--bind` to expose it beyond the loopback interface.

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

Serial commands (newline‑terminated, also usable from any terminal): `band 5g|2.4g|both|ble|154`, `park <ch>|0`,
`cap 0|1`, `sdcap 0|1`, `sdinfo`, `sdls`, `sdread <path>`, `time <epoch>`,
`snap <bytes>`, `hunt <mac|ext-addr|pan/short> [ch]` / `hunt 0`, `deauth <bssid>` / `deauth 0` (Wi‑Fi
modes only — parks on the AP's channel and kicks its stations, auto‑stops after 5 min), `info`, `reboot`.
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

## Tuning knobs (`bandwatch/bandwatch.cpp`)

`kChannels[]`, `kDwellMs` (220), `kStrongThresholdDbm` (−65), `kBusyEmaAlpha` (0.22), `kLongPressMs` (700),
`kCapSlots` / `kCapMaxLen` (capture ring: 20 × 1600 B), `kCountryCode` ("EU", only affects the regulatory table),
`kDeauthMaxMs` (5 min, the deauth dead‑man's switch), `kSdCsPin` (4), `kSdSpiHz` (20 MHz), `kSdBufSize` (4 KB,
matches the FATFS sector size), `kSdFlushMs` (5 s), `kSdBudgetUs` (8 ms of SD writing per loop).

## Known issues

**The deauth attack does not work** (as of 1.2.5, verified on hardware against a Ubiquiti AP). Frames are
built and handed to the driver, the counters climb, no errors are returned — and no station is ever kicked.

What was measured, on a real AP with a station of ours associated to it at −62 dBm:

- The AP's own advertised client count (BSS-load IE) never changed during an attack, and the associated
  station never dropped. Tried on a DFS channel (64) and a non-DFS channel (2) — no effect on either.
- **PMF is not the explanation**: the target advertises PMF `none`.
- **DFS is not the explanation**: it fails identically on a non-DFS channel.
- The `deauth`/`da` counters mean "frames we handed to the driver", *not* frames that reached the air:
  `ic_tx_pkt()` returns `void`, and `esp_wifi_80211_tx()` returning `ESP_OK` only means the frame was
  accepted for queueing. Do not read a rising counter as a working attack.

One real bug was found and fixed on the way: the frame was not a deauthentication frame at all. The code
overwrote the driver's frame control with `0xC8 0x02` (type 2 / subtype 12 = a **QoS-Null data frame**), and
the raw fallback used `0x80` (**Beacon**) and `0xD0` (**Action**). All are ignored by stations. The frame is
now a correct `0xC0 0x00` deauthentication, confirmed by dumping the bytes handed to the MAC — but fixing it
did not make the attack work, so at least one further cause remains unidentified.

Whether anything is radiated at all is **still unverified in both directions**: the only witness available
during testing was a macOS Wi-Fi scan, and macOS redacts SSIDs in `system_profiler` output, so a
beacon-injection self-test (`txtest`) could not be read. Confirming this needs a second radio that can see
raw 802.11 — another ESP32 in promiscuous mode, or a USB adapter in monitor mode. See
[`docs/DEVELOPER.md`](docs/DEVELOPER.md) §11 for the full investigation and the next steps.

## Limitations

- One radio: Wi‑Fi bands, Bluetooth LE and 802.15.4 are exclusive modes. No Bluetooth Classic on the C5. The
  prebuilt BLE stack scans legacy advertisements only (Bluetooth 5 extended / coded‑PHY advertising is compiled
  out of the Arduino core). 802.15.4 protocol detection is heuristic (network-layer header bytes); Zigbee/Thread
  payloads are encrypted on the air and stay encrypted here.
- RAM: 320 KB with no PSRAM. Only the visible LCD page exists as widgets; the capture ring is allocated per
  capture; BLE scan cycles are short and cut early when heap runs low (~28 KB).
- The busy score is a traffic proxy, not calibrated airtime; thresholds were tuned on 2.4 GHz, so a busy
  802.11ac/ax channel may read a little high.
- DFS channels are received passively; any channel the driver refuses is skipped and marked `x`.
- Not a replacement for professional RF tools.

## Versions

- **1.2.4** — full-codebase review pass. Firmware: a mode change now frees the capture ring instead of only
  switching capture off (it used to hold ~32 KB through BLE mode, where RAM is tightest); SSIDs, BLE names and
  country codes are stripped of control characters at ingest, and the country code is JSON-escaped like every
  other string, so a malformed beacon can no longer corrupt a `{"t":"w"}` line; a bogus RSN cipher count can no
  longer wrap the IE parser's offset; `stopDeauth()` restores the quiet-sniffing TX power; a `#warning` fires if
  the Arduino core is not the 3.3.x the deauth offsets were reverse-engineered against. Host: a malformed line
  no longer tears down the serial session and silently ends a capture; stopping a busy capture no longer races
  the pcap writer; changing band with the BOOT button now closes the host's pcap too; `/api/cmd` returns 400 on
  a bad argument instead of raising.
- **1.2.3** — deauth hardening: fixed a null-pointer crash in the driver-internal kick path
  (`sendInternalKick`) that could fire on essentially every attack, added a 5‑minute dead‑man's‑switch
  so an attack stops itself if the host/serial link drops, dropped leftover FC-probe diagnostics from
  the raw-frame fallback path, and exposed a failed-frame counter (`deauth` now reports
  `[bssid, ch, sent, failed]`, shown on the dashboard's deauth card). Host: command strings sent over
  serial (`deauth`/`hunt`/`park`/...) now have embedded newlines stripped so an API value can't smuggle
  a second command onto the line.
- **1.2.2** — deauth attack: `deauth <bssid>` parks on the AP's channel and spams spoofed deauth frames until its
  stations drop (run a capture alongside to catch WPA2 handshakes); *Deauth* button per AP row in the dashboard,
  frame‑counter card while it runs.
- **1.2** — 802.15.4 (Zigbee / Thread) sniff mode with node table and pcap, Zigbee/Thread dashboard tab, pause /
  hover‑freeze for device tables, `reboot` command, developer docs.
- **1.1** — Bluetooth LE mode, Wi‑Fi/BLE device tables with beacon and advertisement details, hunt mode, Devices
  and Hunt LCD pages, dashboard tabs with vendor lookup.
- **1.0** — 5 GHz / 2.4 GHz / both sweeping, three LCD pages on the BOOT button, serial protocol, host dashboard
  with pcap capture.
