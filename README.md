# Bandwatch — Waveshare ESP32-C5-LCD-1.47

A Wi‑Fi **activity** meter and device finder for the dual‑band ESP32‑C5. Port of
[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD)'s *Bandwatch* (2.4 GHz,
ESP32‑C6) to the **Waveshare ESP32-C5-LCD-1.47**, extended with 5 GHz sweeping, Bluetooth LE scanning, IEEE 802.15.4 (Zigbee / Thread) sniffing, device tables
for all three radios, a "hunt" mode for locating one device by signal strength, an LCD UI driven by the BOOT
button, and a host‑side web dashboard with pcap capture.

Developer documentation (architecture, serial protocol, hardware references, board quirks): [`docs/DEVELOPER.md`](docs/DEVELOPER.md).
A short orientation for AI assistants is in [`CLAUDE.md`](CLAUDE.md).

Bandwatch listens to 802.11 traffic in promiscuous mode and reports a **busy score** (0–100) per channel as a proxy
for channel load. It does **not** measure RF power, true airtime occupancy, or non‑Wi‑Fi interference, and it
never transmits.

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
- **Host dashboard** (`host/bandwatch_host.py`): Overview tab (bar chart per channel, trend, table), **Wi‑Fi devices**
  **Bluetooth LE** and **Zigbee / Thread** tabs (sortable, filterable, vendor names from the IEEE OUI registry,
  RSSI sparklines, a *Hunt* button per row), a hunt panel with live RSSI trend, mode/park/capture controls.
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
- **Apple Silicon without Rosetta**: Arduino's bundled `ctags` is x86_64. `tools/ctags/ctags` wraps native
  Universal Ctags (`brew install universal-ctags`, done by `setup.sh`).

## Host dashboard and pcap capture

```sh
python3 host/bandwatch_host.py            # http://127.0.0.1:8080 , pcaps in ./captures
python3 host/bandwatch_host.py --port /dev/cu.usbmodem21101 --http 8080 --captures ~/pcaps
```

The page polls the device state once a second: band and park controls, capture start/stop, stat tiles, a busy‑score
bar chart per channel (grouped by band segment, current channel marked), a 10‑minute trend of the max score, a
per‑channel table and the device log.

**Capture** streams every received frame (up to the snap length) over USB; the host writes
`captures/bandwatch-YYYYmmdd-HHMMSS.pcap` with a radiotap header (TSFT, channel, dBm signal) that Wireshark
opens directly. Park on one channel for a continuous capture; while hopping, each sweep contributes a 220 ms slice
per channel. Throughput is bounded by USB CDC (~300 KB/s of frame data); the device counts frames it had to drop
(`drop` in the dashboard). Frames are captured as received, i.e. encrypted payloads stay encrypted. If Wireshark
reports bad FCS on every frame, run with `--no-fcs`.

Serial commands (newline‑terminated, also usable from any terminal): `band 5g|2.4g|both|ble|154`, `park <ch>|0`,
`cap 0|1`, `snap <bytes>`, `hunt <mac|ext-addr|pan/short> [ch]` / `hunt 0`, `info`, `reboot`.
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
`kCapSlots` / `kCapMaxLen` (capture ring: 20 × 1600 B), `kCountryCode` ("EU", only affects the regulatory table).

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

- **1.2** — 802.15.4 (Zigbee / Thread) sniff mode with node table and pcap, Zigbee/Thread dashboard tab, pause /
  hover‑freeze for device tables, `reboot` command, developer docs.
- **1.1** — Bluetooth LE mode, Wi‑Fi/BLE device tables with beacon and advertisement details, hunt mode, Devices
  and Hunt LCD pages, dashboard tabs with vendor lookup.
- **1.0** — 5 GHz / 2.4 GHz / both sweeping, three LCD pages on the BOOT button, serial protocol, host dashboard
  with pcap capture.
