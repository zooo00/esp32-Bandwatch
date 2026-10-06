# Bandwatch — Waveshare ESP32-C5-LCD-1.47

A Wi‑Fi **activity** meter and device finder for the dual‑band ESP32‑C5. Port of
[PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD)'s *Bandwatch* (2.4 GHz,
ESP32‑C6) to the **Waveshare ESP32-C5-LCD-1.47**, extended with 5 GHz sweeping, Bluetooth LE scanning, IEEE 802.15.4 (Zigbee / Thread) sniffing, device tables
for all three radios, a "hunt" mode for locating one device by signal strength, a deauth attack that kicks
an AP's clients off their network, an LCD UI driven by the BOOT button, and a host‑side web dashboard with
pcap capture.

Developer documentation (architecture, serial protocol, hardware references, board quirks): [`docs/DEVELOPER.md`](docs/DEVELOPER.md).
A short orientation for AI assistants is in [`CLAUDE.md`](CLAUDE.md). Planned work, open questions and known
gaps are tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md).

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
./host/run-v2.sh                          # the new dashboard at "/" (classic moves to /classic)
```

The page polls the device state once a second: band and park controls, capture start/stop, stat tiles, a busy‑score
bar chart per channel (grouped by band segment, current channel marked), a 10‑minute trend of the max score, a
per‑channel table and the device log.

A second layout ships alongside it (`dashboard2.html`, always served at `/v2`): controls grouped into a left rail,
one scrolling column instead of tabs, only the active radio's device table open — the other two fold away as
"last seen" caches — and hunt/deauth sharing one action bar. `--ui v2` (or `run-v2.sh`) puts it at `/`; both pages
cross‑link, so you can flip between them without restarting until you are ready to promote one. The new page also
shows the SD pull flow: every file on the card gets a *pull to Mac* button that streams it back over serial and
offers a download.

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
modes only — broadcast deauth to all clients of that AP, auto‑stops after 5 min), 
`dca <client_mac> <ap_bssid>` / `dca 0` (targeted deauth to one specific station), 
`info`, `reboot`.
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
`kCapSlotsMax/Min` / `kCapMaxLen` (capture ring: up to 20 × 1600 B, sized down while the card is mounted),
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

**Whether a real station actually disconnects is untested.** Correct deauth frames demonstrably reach the air;
no client of ours has been observed dropping. PMF-enabled networks ignore these frames by design.

## Limitations

- One radio: Wi‑Fi bands, Bluetooth LE and 802.15.4 are exclusive modes. No Bluetooth Classic on the C5. The
  prebuilt BLE stack scans legacy advertisements only (Bluetooth 5 extended / coded‑PHY advertising is compiled
  out of the Arduino core). 802.15.4 protocol detection is heuristic (network-layer header bytes); Zigbee/Thread
  payloads are encrypted on the air and stay encrypted here.
- RAM: 320 KB with no PSRAM. Only the visible LCD page exists as widgets; the capture ring is allocated per
  capture; BLE keeps no result cache of its own, so BLE mode idles at ~95 kB free.
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
investigation, and a rebuilt host dashboard. See [Versions](#versions) for the full trail.

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

- **1.12.1** — Public-release housekeeping and a boot-splash fix. The project is now **MIT-licensed**
  ([`LICENSE`](LICENSE) + [`NOTICE`](NOTICE)) with the carve-out that the upstream
  [PierreGode/WaveshareESP32C6LCD](https://github.com/PierreGode/WaveshareESP32C6LCD) base it derives from
  ships no license of its own, so the MIT grant covers only this project's own work; a README **Credits**
  section records the C6→C5 port lineage. `build.sh` now auto-regenerates `bandwatch/boot_logos.h` from
  `bootlogo/boot_*.png` whenever the pictures change (header missing, a PNG newer than it, or a count that no
  longer matches `K_BOOT_LOGO_COUNT`; `LOGOS=always`/`never` override). **Fix:** committing a mode card now
  hides the boot-photo overlay, so tapping BOOT during a committed-mode splash — e.g. after releasing the
  hold-walk on Spectrum, before the splash hands off to the scan page — no longer falls through into the
  picture loop. `showBandSplash()` drew the card over the logo but never hid it, and `logoActive()` only
  tested the hidden flag, so a photo shown earlier in the walk stayed "active" underneath.
- **1.12** — **Boot photos.** Eight pictures from `bootlogo/` are embedded in the firmware as RGB565 (~860 KB of flash,
  no RAM cost - LVGL draws the const arrays straight from flash). A random one shows full-screen for two seconds at boot
  before the mode card; the BOOT hold-walk gains a photo stop after Spectrum - release there and it lingers five seconds
  while taps step through the pictures in order. Tapping past the last page flashes one on the way around too.
  Regenerate `bandwatch/boot_logos.h` after changing the images:
  `python3 tools/img2c.py bandwatch/boot_logos.h 172 320 bootlogo/boot_*.png`.
- **1.10** — **Mode splash on the LCD.** A band change - button, host command or boot - flashes a full‑screen name
  card for the new mode before its scan page takes over ("5 GHz / Wi-Fi ch 36‑165", "Spectrum / raw 2.4 GHz energy").
  Holding BOOT walks the modes' cards one by one at an even 700 ms cadence; release commits the one on screen (holding
  from the hunt page stops the hunt first, then keeps walking). With it came a layout pass measured against the real
  font widths: header right labels shrank so "park 165 USB" no longer collides with the title, the main page's
  product‑name title became *Activity*, Top‑3 rows widened (a wrapped "ch149 75" used to spill into the row below), a
  maxed score stays inside its channels‑grid column, and dual‑wifi reads `BOTH chNN` instead of a flipping 2.4G/5G
  prefix that looked like single‑band mode.

- **1.9** — **Dashboard v2, and a review pass.** A fresh dashboard layout (`dashboard2.html`: left‑rail controls,
  one scrolling column instead of tabs, the active radio's device table open while the other two fold into "last seen"
  caches, hunt/deauth sharing one action bar) ships in parallel — `--ui v2` or `./host/run-v2.sh` puts it at `/`, and
  both pages cross‑link. The host finally reassembles the `sdread` file chunks it had been dropping into the log pane:
  `S <n> <base64>` lines now become a real pcap under `captures/`, served back for download (the new UI's pull button).
  Firmware review fixes: the internal‑kick deauth sequence now encodes like its outer path — the old bytes put the
  sequence's low nibble into the *fragment* field and repeated every 256 frames; `finishDwell()` lost the spec branch
  left unreachable by v1.8 (in fine spectrum, `currentIdx` indexes the bin grid, not the channels); Wi‑Fi/BLE hunt hits
  check `kind == 0`, so a stale MAC from an earlier hunt can no longer light up a 15.4 key hunt; and `cap`/`sdcap` are
  refused in spectrum mode with a reason line, where no RX is armed (an empty file growing forever).

- **1.8** — **Fine‑resolution spectrum.** `spec` mode leaves the fixed 16‑bin grid: the sweep now walks
  2400–2483 MHz at a selectable 1/2/5 MHz step (`specstep`, on the LCD and in the dashboard), with a per‑dwell cursor,
  max‑hold bins, and a waterfall that finally resolves sub‑channel emitters.

- **1.7** — **The spectrum analyzer.** A `spec` mode reads true 2.4 GHz RF power (dBm) from the 802.15.4 energy
  detector across channels 11–26, and the host flags bins whose sustained energy no recently‑decoded Wi‑Fi/BLE/Zigbee
  device explains — evidence of an emitter this radio can't demodulate, not an identification. The 1.7.x patches fixed
  the readings themselves: a reverted loop‑driven hop (the ED window was being re‑armed too often to integrate),
  widened windows so the spectrum tracks the Wi‑Fi band's shape, RF‑energy colours that read on both themes, and a
  receiver‑blocked warning when every frequency's noise floor is elevated.

- **1.6** — **The deauth attack transmits.** Settled with an external witness (`tools/witness/`: an ESP32-S3
  in monitor mode that never transmits, plus `verify.py` to drive both boards and report from the air). Two
  unrelated faults had been hiding each other: the raw `esp_wifi_80211_tx()` path was rejected before TX by
  `libnet80211`'s subtype gate, and the driver-internal slot path — which was the default, and which §11 had
  called "the real attack path" — radiates **nothing for any subtype**, so the §9 offsets are wrong rather than
  fragile. Raw TX is now the default (it fails loudly rather than silently), and
  `tools/deauth/patch_raw_tx.py` patches out the gate between compile and flash (applied by `build.sh` by
  default, `--no-patch` to opt out), resealing the image checksum and SHA-256. 289 deauth frames witnessed on air at −38 dBm. Also: `kickfc` and `kickpath` diagnostics, and
  `-Wl,--wrap` ruled out for good (the check and its caller share an object file, on every ESP32 family).

- **1.5.5** — **Merge of the two 1.5.x lines.** 1.5.3 and 1.5.4 were developed from a checkout that predated
  the 1.5.1 module split, so they edited the old monolithic `bandwatch.cpp`. Targeted deauth (`dca`) and the
  station-to-BSS association were ported onto the module layout: attack state into `Deauth` in
  `bandwatch_core.h`, `startDeauthTargeted()` and the DA steering into `deauth_diag.cpp`, the `dca` command
  and its acknowledgement into `host_proto.cpp`, the DS-bit association into `wifi_sniff.cpp`. Two behaviour
  differences fell out of the merge: the raw-TX fallback now honours targeted mode (it would have broadcast to
  the whole BSS when the driver-internal slot was unavailable — worse than doing nothing), and `sdls` keeps
  1.5.1's `total`/`sent` reporting rather than 1.5.4's `listed`/`skipped`, since both fixed the same gap.
  Static RAM 79,592 B. **The 1.5.3 and 1.5.4 tags predate the split and do not build from this tree.**
  As with every deauth path here, **the attack still does not work** ([`docs/DEVELOPER.md`](docs/DEVELOPER.md) §11).
- **1.5.4** — **The targeted deauth from 1.5.3 could not actually be reached.** Four separate breaks between
  the dashboard and the device, any one of them fatal: the client picker filtered on a `parent` field nothing
  ever produced, so it was always empty; its buttons were emitted with `display:none` and had no styling or
  handler to reveal them; the client MAC was sent with its colons stripped, which the device's parser rejects,
  so a targeted kick *stopped* the running attack instead of starting one; and the host never dispatched the
  `dca` acknowledgement, so a targeted attack left no trace in the UI at all. All four fixed, and the missing
  piece underneath them built: the firmware now tracks **which BSS each station is on**, read from the DS bits
  of data frames, so the Wi‑Fi table shows *on &lt;network&gt;* per station and the picker has real clients in
  it. That association cost **zero** static RAM — `WifiDev` was repacked from 64 bytes-with-padding to 64
  bytes-exactly ([`docs/DEVELOPER.md`](docs/DEVELOPER.md) §17).
  Also: a targeted deauth whose client MAC began `00:00` was silently demoted to a broadcast kick (the
  "is this targeted?" test read two bytes of a MAC); starting a deauth before the Wi‑Fi driver had an
  `ieee80211com` dereferenced a null pointer and panicked; the deauth channel lookup walked the device table
  without its lock; the System page named the AP instead of the station being kicked; `sdls` now reports how
  many entries it had to omit instead of silently shortening the listing (superseded in the 1.5.5 merge by
  1.5.1's `total`/`sent`, which fixed the same gap); `hello` reserved ~10 bytes less
  serial room than its longest possible line; and the HTTP API now rejects malformed MACs rather than
  forwarding them.
- **1.5.3** — **Targeted deauth (`dca <client_mac> <ap_bssid>`).** `deauth <bssid>` kicks every station on a
  network; `dca` sends the deauthentication to one station only, with the AP's BSSID as SA/BSSID and the
  client as DA. Reachable from the dashboard only as of 1.5.4 — see above. As with every deauth path here,
  **the attack still does not work** and the counters do not tell you otherwise
  ([`docs/DEVELOPER.md`](docs/DEVELOPER.md) §11).
- **1.5.2** — **Deauth sequence numbers.** Every frame in a burst carried sequence number `0x00`, so a
  receiver could treat the whole burst as one retransmitted frame. Each frame now gets a unique sequence, and
  the TX path logs which branch it took (home-channel vs deferred queue, alloc failures, hstate) to narrow
  down §11. The attack still does not work; the root cause is still unidentified.
- **1.5.1** — **Review pass.** The firmware sketch was split into modules behind one shared header
  (`bandwatch_core.h`): core, the three radios, deauth/diagnostics, capture, SD, host protocol and LCD UI are
  now separate translation units. A capture is refused below a 24 kB free-heap floor rather than crashing on
  allocation, and `sdls` reports `total`/`sent` so a listing truncated by a full serial buffer is visible as
  truncated ([`docs/DEVELOPER.md`](docs/DEVELOPER.md) §3 and §16).
- **1.5** — **Surveillance-hardware flagging and receiver-side sightings.** 64 OUI prefixes across 7
  categories (Flock Safety, Ring, Axon, DJI, Parrot, Skydio, Meta/Ray-Ban) are matched in firmware, so hits
  show on the LCD (`!`, orange) as well as the dashboard (red badge, plus a *surveillance only* filter).
  Wi-Fi tracking now also records devices seen only as a frame **destination** (`addr1`) — the technique that
  surfaces cameras which sleep through a dwell window — tiered as weaker evidence so they never evict or
  overwrite a device we actually heard (`addr1 0|1`). `sdread` is incremental now, so pulling a capture no
  longer stalls channel hopping. An OUI match is evidence, not proof; see
  [`docs/DEVELOPER.md`](docs/DEVELOPER.md) §15.
- **1.4.2** — SD recording worked but *looked* broken from the dashboard: BLE mode emits no dwell lines, so the
  capture counters never reached the host, and the `sdcap` ack used a key the host was not reading. Both fixed.
  Adds a record light on the LCD: the page header turns **red** and shows `USB` / `SD` / `REC` while recording.
- **1.4.1** — fixes a BLE address regression from 1.4: `ble_addr_t.val` is little-endian, and feeding it
  straight to the device table byte-reversed every BLE MAC, which also broke OUI lookup and the
  random-address bit. Display paths now get MSB-first order while the pcap keeps the on-air bytes. Adds the
  **auto** scan policy (passive, with a short active burst when a new scannable device appears, rate-limited
  and frozen during recording), a Passive/Auto/Active switch on the dashboard, and an LCD scan-mode indicator.
- **1.4** — **BLE advertising capture.** Records BLE to pcap as `LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR` (256),
  verified in Wireshark (2276/2276 packets, zero malformed). Required replacing the Arduino `BLEScan` wrapper
  with direct NimBLE `ble_gap_disc()`, since the wrapper merges advertisement payloads and cannot give
  per-packet data. Side benefit: no result cache, so BLE mode idles at ~95 kB free instead of ~58 kB. Adds
  `blescan passive|active`. Approach studied from
  [ouispy-blesniff](https://github.com/shermanatoor/ouispy-blesniff).
- **1.3** — **microSD pcap recording.** The device writes pcap itself (`sdcap`), byte-compatible with the host
  writer and verified with Wireshark's `capinfos`. Independent USB and SD sinks share one capture ring, so both
  can run at once. `sdls` / `sdread` retrieve files over serial; `time <epoch>` (sent by the host on connect)
  gives real timestamps and filenames. The card is mounted only while in use, because FATFS costs ~30 kB and
  BLE mode needs that headroom. Dashboard gains a *Record to SD* button and card status; the LCD System page
  shows recording progress.
- **1.2.5** — the deauth frame was not a deauthentication frame at all: the code overwrote the driver's frame
  control with `0xC8 0x02`, a QoS-Null **data** frame that every station ignores, and the raw fallback used
  Beacon and Action subtypes. Now a correct `0xC0 0x00`, confirmed by dumping the bytes handed to the MAC —
  but the attack still does not work, and PMF, DFS and promiscuous mode are all ruled out. See
  [Known issues](#known-issues) and [`docs/DEVELOPER.md`](docs/DEVELOPER.md) §11.
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
