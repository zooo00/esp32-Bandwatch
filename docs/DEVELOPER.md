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
- Reference build (core 3.3.11, lvgl 9.3.0): 0 errors, 0 warnings, **1 812 457 B flash (57 % of the 3 MB app
  partition)** and **78 960 B static RAM (24 %)** — that static figure is close to the ~80 KB ceiling in
  `CLAUDE.md` rule 4, so watch it when adding globals.
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

## 3. Firmware architecture (`bandwatch/bandwatch.cpp`)

Single-file design on purpose (one core, tight RAM, easy to read top to bottom). Sections, in file order:

1. **Tunables & tables** — dwell (220 ms), thresholds, `kChannels[]` / `kChanBand[]` (13 × 2.4 GHz, 25 × 5 GHz,
   16 × 802.15.4), `BandMode` enum (`5g`, `2.4g`, `both`, `ble`, `154`), capture ring limits, LCD page enum.
2. **State** — per-channel `ChannelState` (last dwell metrics, raw score, EMA score), the shared `Accum` written by
   the RX paths under `g_accumMux`, the device tables (`wifiDevs`, `bleDevs`, `devs154`) under `g_devMux`, hunt
   target, capture ring pointers, mode flags.
3. **Wi‑Fi RX path** — `promiscuousCb()` (runs in the Wi‑Fi task): counts frames/bytes/strong, hashes the
   transmitter for the "unique talkers" estimate, calls `trackWifiDevice()` which updates the device table and,
   for beacons/probe responses, runs `parseBeaconIes()` (SSID, RSN/WPA → security bits, HT/VHT/HE/EHT → PHY
   bits, HT/VHT operation → width, BSS load, country). Optionally copies the frame into the capture ring.
4. **BLE** — `AdvCallbacks::onResult()` (NimBLE host task) fills `bleDevs` (address, RSSI, name, company id,
   Apple continuity type byte, appearance, TX power, 16-bit service UUIDs, flags). `startBle()/stopBle()`,
   `serviceBle()` restarts the scan every 3 s (or when heap < 28 KB) because `BLEScan` caches every device seen.
5. **802.15.4** — `esp_ieee802154_receive_done()` (ISR): parses the MAC header (frame type, addressing modes,
   PAN id, short/extended source), aux security header, beacon superframe/GTS/pending fields, classifies the
   upper layer (`classify154`: Zigbee NWK version 2, Green Power version 3, 6LoWPAN dispatch bytes, MAC-level
   security ⇒ Thread-style), then `track154()`. `start154()/stop154()`.
6. **Channel control / lifecycle** — `applyChannelIdx()` (Wi‑Fi `esp_wifi_set_channel`, or
   `esp_ieee802154_set_channel` + `receive`), `advanceChannel()` (skips disabled/rejected channels, honours
   `parkedIdx`), `startWifi()/stopWifi()` (country with manual policy + full 5 GHz mask, band mode, protocols
   incl. 11ax, promiscuous filter/callback), `setBandMode()` (the only place radios are swapped).
7. **Scoring** — `computeBusyScore()` (log-scaled pkt/s, B/s, strong ratio, unique talkers; smaller references
   in 802.15.4 mode), EMA α = 0.22, `globalActivityMax()`, `sortTop3()`.
8. **Host protocol** — `sendHello/sendDwell/sendSweep/sendBleStatus/sendDevices`, base64 frame streaming
   (`drainCapture`, bounded per loop iteration), `handleCommand()` / `pollSerial()`. All output goes through
   `serialRoom()` so a line is either written whole or skipped.
9. **Dwell/hop** — `hopIfNeeded()` called from the LVGL timer every 120 ms: finishes the dwell (snapshot the
   accumulator, score, EMA, emit `d` line), hops, emits `s` after a wrap-around.
10. **UI** — LVGL 9, 172×320 portrait. `showPage()` deletes the current page's widgets and builds the new one
    (`buildOverviewPage`, `buildChannelsPage`, `buildDevicesPage`, `buildHuntPage`, `buildSystemPage`);
    `refreshUi()` updates only the visible page and drives the LED. `pollButton()` = tap → next page, hold 0.7 s →
    next mode (or stop hunt on the Hunt page).
11. **Entry points** — `Bandwatch_Init()` (called from `Lvgl_Init` in `LVGL_Driver.cpp`) and `Bandwatch_Loop()`
    (called from `loop()`): serial commands, button, capture drain, BLE cycle, periodic device/status lines.

Tasks and contexts: Arduino `loop` task (LVGL + everything in `Bandwatch_Loop`), Wi‑Fi task (promiscuous
callback), NimBLE host task (advert callback), 802.15.4 ISR. Shared data is protected with the two spinlocks;
never call anything that allocates or blocks inside them.

Memory budget (measured, v1.2): static ≈ 79 KB; free heap ≈ 100 KB in Wi‑Fi modes, 60–100 KB in BLE mode,
≈ 120 KB in 802.15.4 mode; capture ring = up to 20 × 1.6 KB allocated on `cap 1`.

## 4. Serial protocol (USB CDC, 115200, newline-delimited)

Device → host, one JSON object per line unless noted:

| Line | When | Fields |
| --- | --- | --- |
| `{"t":"hello",...}` | boot, `info`, after `band` | `fw`, `ver`, `dwell_ms`, `band` (mode), `country/bandmode/proto/promisc` (esp_err names), `chs` (channel list of the mode), `park`, `cap`, `snap`, `heap`, `up` (s), `rst` (reset reason), `hunt` (id or null), `h` (hunt status `[rssi, age_ms, hits]` or null), `deauth` (`[bssid, park ch (0 if hopping), frames sent, frames failed]` or null), `sd` (`{mounted, mb, cap, file, frames, bytes, err, clock}`) |
| `{"t":"d",...}` | every completed dwell | `c` channel, `s` EMA score, `r` raw score, `f` frames, `b` bytes, `st` strong, `u` unique, `g` global max, `n` sweep no., `park`, `cap`, `drop` (capture drops), `da` (deauth frames sent so far; 0 when idle), `df` (deauth frames failed so far; 0 when idle), `sdc` (1 while recording to microSD), `sdf`/`sdb` (frames/bytes written to the card), `h` |
| `{"t":"s",...}` | after every full sweep | `n`, `g`, `band`, `ch`: `[[ch, ema, frames, bytes, strong, unique, state], ...]` (state 0 ok / 1 no data / 2 rejected), `aps`, `drop`, `heap` |
| `{"t":"w","dev":[...]}` | every 2 s in Wi‑Fi modes | rows `[mac, rssi, max, frames, age_ms, ch, flags, ssid, sec, pmf, phy, bw, util, stations, cc]`; flags bit0 AP, bit1 IEs parsed; `sec` bits: 0x01 WEP, 0x02 WPA, 0x04 WPA2‑PSK, 0x08 WPA2‑Ent, 0x10 WPA3‑SAE, 0x20 WPA3‑Ent, 0x40 OWE, 0x80 open; `pmf` 0/1/2; `phy` bits 1 legacy, 2 n, 4 ac, 8 ax, 16 be; `bw` in 10 MHz units; `util` 0–255; `cc` country |
| `{"t":"b","dev":[...]}` | every 2 s in BLE mode | rows `[mac, rssi, max, adverts, age_ms, addrType, company, name, appearance, txPower(127=none), svcUuid16, svcDataUuid16, appleType, flags]`; flags bit0 connectable, bit1 legacy adv, bit2 scannable |
| `{"t":"z","dev":[...]}` | every 2 s in 802.15.4 mode | rows `[id, rssi, max, frames, age_ms, ch, pan, short, proto, flags, lqi]`; `id` = extended address `aa:bb:cc:dd:ee:ff:00:11` or `pan/short` hex; `proto` 0 unknown, 1 Zigbee, 2 Zigbee GP, 3 Thread/6LoWPAN, 4 MAC‑secured; flags bit0 ext addr, bit1 beacons, bit2 permit join, bit3 MAC security, bit4 data seen |
| `{"t":"ble",...}` | every 1 s in BLE mode | `devs`, `cycles`, `heap`, `adv` (advertising reports), `scan` (policy) / `running` (what is actually running) / `switches`, `cap`, `drop`, `sdc`/`sdf`/`sdb`, `h`. **BLE mode emits no dwell lines, so this is the only live capture telemetry there** - anything added to `{"t":"d"}` for the dashboard has to be added here too |
| `{"t":"ack",...}` / `{"t":"log","msg"}` / `{"t":"err","msg"}` | command replies and notices | |
| `S <n> <base64>` | after `sdread <path>` | one chunk of a file being streamed off the card; bracketed by `sdread` / `sdread_done` acks |
| `{"t":"sdls","files":[[name, bytes], ...]}` | after `sdls` | files in the card root |
| `P <ch> <rssi> <ts_us> <len> <base64>` | while `cap 1` | one captured frame; `len` = original length, payload may be truncated to the snap length. Wi‑Fi frames include the FCS; 802.15.4 frames exclude it |

Host → device commands: `band 5g|2.4g|both|ble|154`, `park <ch>` / `park 0`, `cap 1|0`, `snap <32..1600>`,
`hunt <mac> [ch]` / `hunt <ext addr>` / `hunt <pan>/<short>` / `hunt 0`, `deauth <bssid>` (Wi‑Fi modes only —
parks on the AP's channel and spams spoofed deauth frames at its stations; stops itself after `kDeauthMaxMs`,
5 min) / `deauth 0`, `sdcap 0|1`, `sdinfo`, `sdls`, `sdread <path>`, `time <epoch>`, `info`, `reboot`.

The device drops a whole line rather than truncating it, so the host must tolerate missing lines — but it must
also tolerate *malformed* ones: `handle_line()` wraps the dispatch so a short or unexpected line is logged
instead of killing the reader thread (an exception there closes the serial port and silently ends a capture).

## 5. Host tool (`host/`)

`bandwatch_host.py`: a reader thread parses lines into a state dict (channels, history, device tables with
per-device RSSI history, hunt state), an HTTP server exposes `GET /api/state` (everything, JSON) and
`POST /api/cmd` (`{"cmd":"band"|"park"|"capture"|"hunt"|"deauth"|"info", ...}`), and `PcapWriter` writes radiotap pcaps
for Wi‑Fi and 802.15.4‑TAP pcaps for 802.15.4. Files are named `bandwatch-wifi-YYYYmmdd-HHMMSS.pcap` /
`bandwatch-802154-…` in `--captures` (default `./captures`).

`/api/cmd` has **no authentication**, and one of its commands starts a deauth attack, so the server binds to
`127.0.0.1` by default; `--bind 0.0.0.0` hands that to anyone who can reach the port. Values that reach the
serial line have embedded CR/LF stripped in `Bandwatch.send()` so a crafted field cannot append a second
command to the line. The pcap writer is touched from both the reader thread and the HTTP thread, so
`handle_frame()` takes a local reference and tolerates the file being closed underneath it.

Vendor names: IEEE OUI CSV cached in `~/.cache/bandwatch/oui.csv`
(downloaded once in the background) with a built-in fallback; Bluetooth company ids, Apple continuity types,
GAP appearance categories and common service UUIDs are small tables at the top of the file.

`dashboard.html`: no framework, polls `/api/state` once a second. Tabs: Overview (bar chart, trend, channel
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
- **SD card logging**: unused so far; SPI bus is shared with the LCD (CS GPIO4), upstream `SD_Card.cpp` shows
  the Arduino SD usage.

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
whose structs moved will silently corrupt memory instead of failing. A `#warning` in `bandwatch.cpp` fires if
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
- `devSnap` (the union scratch buffer) and `devRows` are loop-task only — `sendDevices()` and `refreshDevices()`
  cannot interleave, since both run from `loop()`.
- The capture ring is single-producer (radio) / single-consumer (loop). Freeing it from the loop task
  (`releaseCapture()`) is safe **only** because this is a single-core part: the radio callbacks re-read
  `captureEnabled` and `capRing` on entry and run to completion, so they can never be suspended holding a
  pointer that the loop task then frees. Do not move that free anywhere else, and do not cache `capRing` in a
  local inside the producers.
- Strings captured off the air (SSID, BLE name, country code) are stripped of control characters at ingest
  (`sanitizeText`) so `printJsonStr` cannot expand them into `\u00xx` escapes that overshoot the `serialRoom()`
  budget for a line. The budgets (`40 + n*110` Wi‑Fi, `40 + n*95` BLE) are estimates, not exact lengths: a full
  64-device table already needs ~7 KB of the 8 KB TX buffer, so raising them is not free.

## 11. Deauth: why it does not work (investigation log, 1.2.5)

Read this before spending another evening on the deauth path. Tested on hardware against a Ubiquiti AP
(WPA2-PSK, 5 GHz ch 64 80 MHz, and its 2.4 GHz ch 2 BSS) with a laptop associated to it at −62 dBm.

**Symptom.** Frames are built, handed to the driver and counted; no station is ever kicked. The AP's
BSS-load client count does not move and the associated laptop stays connected.

**Ruled out** (each tested, not assumed):

| Hypothesis | How it was tested | Result |
| --- | --- | --- |
| PMF / 802.11w protecting the BSS | read the RSN capabilities out of the beacon | PMF is `none` — not it |
| DFS blocking TX on channel 64 | ran the identical attack on non-DFS ch 2 | fails identically — not it |
| Promiscuous mode blocking TX | `esp_wifi_set_promiscuous(false)` then inject | no change — not it |

**One real bug, found and fixed.** `sendInternalKick()` let the driver build a proper deauth and then
overwrote the frame control with `0xC8 0x02`. That decodes as type 2 / subtype 12 — a **QoS-Null data
frame**, which every station ignores. The raw fallback was no better: `0x80` is a **Beacon** and `0xD0` an
**Action** frame (they were chosen to get past `ieee80211_raw_frame_sanity_check`, which rejects real deauth
subtypes — i.e. the check was being satisfied with frames that could never work). Both paths now emit a
correct `0xC0 0x00` deauthentication. Verified by dumping the bytes as handed to the MAC:

```
c0 00  32 00  ff ff ff ff ff ff  78 8a 20 8e 9e f0  78 8a 20 8e 9e f0  00 00  07 00
FC     dur    addr1 DA=broadcast  addr2 SA=AP        addr3 BSSID=AP      seq    reason 7
```

That is a textbook broadcast deauth — and it still kicks nobody, so at least one further cause remains.

**The counters lie.** `deauthSent` (`da`, and element 3 of `deauth`) is incremented at the end of
`sendInternalKick()` unconditionally; `ic_tx_pkt()` returns `void`. On the raw path, `esp_wifi_80211_tx()`
returning `ESP_OK` only means the frame was accepted for queueing. **Neither proves anything reached the
air.** Any future work here needs an external witness before claiming success.

**Unresolved: does the radio transmit at all?** `txtest 1` injects a beacon with SSID `BANDWATCH-TXTEST`,
and `softap <ch>` (proof of concept only, not a feature) brings up an open SoftAP to give the MAC a real BSS
context and injects from `WIFI_IF_AP`. Neither could be evaluated: the only witness to hand was a macOS
Wi-Fi scan, and **macOS redacts every SSID** in `system_profiler SPAirPortDataType` output, so injected
networks cannot be identified by name. A channel+security signature works around the redaction
(an open AP on a quiet channel shows up as `Channel: N` + `Security: None`) but the SoftAP PoC's own state
handling is incomplete, so its result is inconclusive rather than negative. Do not cite it either way.

**Next steps, in order of value:**

1. Get a real witness: a second ESP32 in promiscuous mode, or a USB adapter in monitor mode on the target
   channel. Everything below is guesswork without one.
2. With a witness, settle the open question — does *any* frame radiate? Start with `txtest` (plain STA),
   then the SoftAP PoC. If the SoftAP's own driver-generated beacon is not on the air, the problem is the
   radio/driver configuration, not this code.
3. If frames do radiate but stations ignore them, look at the rate/PHY config
   (`esp_wifi_config_80211_tx()` is pinned to HT20 MCS0) and at whether the sequence number and duration
   survive the driver's TX path.
4. Only then revisit the reverse-engineered offsets in §9.


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
cuts its scan cycles short below `kBleHeapFloor` (28 KB). So the card is mounted **only while in use**:
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
