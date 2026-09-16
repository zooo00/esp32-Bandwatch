# Bandwatch — Waveshare ESP32-C5-LCD-1.47

A Wi‑Fi **activity** meter for the dual‑band ESP32‑C5. Port of
[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD)'s *Bandwatch* (2.4 GHz,
ESP32‑C6) to the **Waveshare ESP32-C5-LCD-1.47**, extended with 5 GHz sweeping, a three‑page LCD UI driven by
the BOOT button, and a host‑side web dashboard with pcap capture.

Bandwatch listens to 802.11 traffic in promiscuous mode and reports a **busy score** (0–100) per channel as a proxy
for channel load. It does **not** measure RF power, true airtime occupancy, or non‑Wi‑Fi interference, and it
never transmits.

## What it does

- **Bands**: 5 GHz (36–64, 100–144, 149–165; 25 channels, ~5.5 s per sweep), 2.4 GHz (1–13), or **both**
  interleaved (38 channels, ~8.4 s). The C5 has one radio, so bands are time‑shared, never simultaneous.
- **Per channel, every 220 ms dwell**: frames, bytes, strong frames (≥ −65 dBm), unique transmitters
  (best effort). Busy score = log‑scaled pkt/s + B/s + strong ratio + talkers, then an EMA (α 0.22).
- **LCD pages** (tap BOOT to cycle, hold BOOT ≈0.7 s to cycle band 5g → 2.4g → both):
  1. *Overview*: max busy score + bar, sweep count, top‑3 channels with pkt/s, per‑channel spectrum strip
     (current channel in cyan), last‑dwell stats for the current channel, transmitter estimate.
  2. *Channels*: every channel of the active band with score bar and value.
  3. *System*: busiest channel details, uptime, sweeps, band, park/capture state, radio status, free heap.
- **RGB LED** mirrors the max busy score (green → yellow → orange → red).
- **USB serial protocol** (JSON lines) for the host tool: live stats, band/park/capture commands, and raw
  802.11 frames streamed as base64 so the Mac writes standard **pcap** files.
- **Host dashboard** (`host/bandwatch_host.py`): live bar chart per channel, trend, table, controls, pcap capture.

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

Serial commands (newline‑terminated, also usable from any terminal): `band 5g|2.4g|both`, `park <ch>|0`,
`cap 0|1`, `snap <bytes>`, `info`.

## Moving to another machine

Everything needed is in this repository. Clone it, run `./setup.sh`, then `./build.sh --upload`. The only
per‑machine state is the Arduino core and libraries that `setup.sh` installs.

## Tuning knobs (`bandwatch/bandwatch.cpp`)

`kChannels[]`, `kDwellMs` (220), `kStrongThresholdDbm` (−65), `kBusyEmaAlpha` (0.22), `kLongPressMs` (700),
`kCapSlots` / `kCapMaxLen` (capture ring: 20 × 1600 B), `kCountryCode` ("EU", only affects the regulatory table).

## Limitations

- One radio: 2.4 GHz and 5 GHz are swept alternately, never observed simultaneously; BLE scanning would have to
  pause Wi‑Fi sniffing (planned as a separate mode).
- The busy score is a traffic proxy, not calibrated airtime; thresholds were tuned on 2.4 GHz, so a busy
  802.11ac/ax channel may read a little high.
- DFS channels are received passively; any channel the driver refuses is skipped and marked `x`.
- Not a replacement for professional RF tools.

## Versions

- **1.0** — 5 GHz / 2.4 GHz / both sweeping, three LCD pages on the BOOT button, serial protocol, host dashboard
  with pcap capture.
- 1.1 (planned) — BLE scan mode.
