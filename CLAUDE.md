# Bandwatch — notes for an AI assistant (or any new developer)

Read `README.md` for what the product does and `docs/DEVELOPER.md` for how it is built. This file is the
short orientation.

## What this is
Firmware for the **Waveshare ESP32-C5-LCD-1.47** board (ESP32-C5, 1.47" ST7789 LCD, WS2812 LED, BOOT button)
plus a Python host tool. The device sniffs Wi-Fi (2.4 + 5 GHz), scans Bluetooth LE, or sniffs IEEE 802.15.4
(Zigbee/Thread), shows results on the LCD, and streams JSON over USB serial to `host/bandwatch_host.py`, which
serves a web dashboard on http://127.0.0.1:8080 and writes pcap files.

## Layout
- `bandwatch/` — Arduino sketch. `bandwatch.cpp` is almost everything (radio control, device tables, host
  protocol, LVGL UI). `devices.h` = device table structs/hash. `Display_ST7789.*`, `LVGL_Driver.*`, `lv_conf.h`
  = display glue (from the upstream C6 project, pins changed).
- `host/bandwatch_host.py` — serial reader, HTTP API, pcap writer, OUI/vendor lookup. `host/dashboard.html` —
  the single-page UI (no build step, no dependencies).
- `build.sh` (compile/flash via arduino-cli), `setup.sh` (install toolchain), `tools/ctags/` (Apple-Silicon
  workaround), `docs/` (developer docs), `captures/` (pcaps, git-ignored).

## Build / flash / run (all from repo root)
```
./setup.sh                      # once per machine
./build.sh                      # compile
./build.sh --upload             # flash (stop the host tool first: it holds the serial port)
python3 host/bandwatch_host.py  # dashboard + pcap
```
Serial console: 115200 baud, but open the port with **DTR and RTS asserted** (see below).

## Hard-won rules for this board (do not relearn these)
1. **DTR/RTS edges reset the chip.** The C5's USB-Serial-JTAG maps DTR/RTS to BOOT/EN. Opening the port with
   pyserial `dtr=False, rts=False` reboots the board; `dtr=True, rts=True` (the macOS default on open) is safe.
   `arduino-cli monitor ... -c dtr=on,rts=on`. Never "toggle to reset" from the host.
2. **After flashing the chip may sit in ROM download mode** ("waiting for download", `boot:0x2e`) because a
   USB-initiated reset does not re-sample the BOOT strap. `build.sh` detects this and issues
   `esptool --before no-reset --after watchdog-reset run`; otherwise press RESET.
3. **Do not run esptool against a running board just to reset it** — its connection attempts toggle DTR/RTS
   repeatedly and reboot the board over and over.
4. **RAM (320 KB, no PSRAM) is the constraint.** Static usage must stay well under ~80 KB. Only the visible LCD
   page exists as LVGL widgets; the capture ring is malloc'ed per capture; LVGL uses `LV_STDLIB_CLIB`; the BLE
   BLE keeps no result cache (we drive NimBLE directly), so BLE idles ~95 kB free. Out-of-memory shows up as
   `abort() ... lock_init_generic` or a store fault in `lv_obj_class_create_obj`.
5. **One radio.** Wi-Fi bands, BLE and 802.15.4 are exclusive modes; `setBandMode()` tears one down and starts
   the next. Every mode change calls `releaseCapture()`: capture off *and* the ~32 KB ring returned to the heap
   (pcap link type differs per radio, and BLE mode needs that RAM). Only free the ring from the loop task.
6. **Serial output never blocks** (`setTxTimeoutMs(0)`, 8 KB TX buffer) and every line first checks
   `Serial.availableForWrite()` so lines are dropped whole, never truncated.
7. **The prebuilt Arduino core cannot be reconfigured** (sdkconfig is fixed): BLE extended advertising is off,
   802.15.4 is on, 5 GHz Wi-Fi is on. Changing that means switching to ESP-IDF.
8. **Do not bump the core casually.** The deauth path pokes hard-coded offsets inside the prebuilt
   `libnet80211.a` (core 3.3.11 / IDF 5.5.5). Nothing checks them at runtime, so a different layout corrupts
   memory instead of failing. A `#warning` fires off 3.3.x; re-verify the offset table in `docs/DEVELOPER.md` §9.
9. **Strings off the air are hostile input.** SSID / BLE name / country code are control-character-stripped at
   ingest and JSON-escaped on the way out; keep both, or a crafted beacon corrupts a whole protocol line.
10. **The deauth attack does not work** (verified on hardware, 1.2.5) and the counters do not tell you that:
   `deauthSent`/`da` counts frames handed to the driver, `ic_tx_pkt()` returns `void`, and an `ESP_OK` from
   `esp_wifi_80211_tx()` only means "queued". PMF and DFS have been ruled out; the frame itself is now a
   correct `0xC0 0x00` deauth. Never claim it works without an external witness (a second radio in monitor
   mode). Full log and next steps: `docs/DEVELOPER.md` §11. Note macOS redacts SSIDs in
   `system_profiler SPAirPortDataType`, so a Mac Wi-Fi scan cannot identify an injected network by name.
11. **The SD card shares the LCD's SPI bus** (CS GPIO4, 20 MHz vs the LCD's 40 MHz). Safe only because both
   wrap transfers in beginTransaction/endTransaction and both run on the loop task — never touch the card
   from a radio callback or another task. Mounting FATFS costs ~30 KB, so the card is mounted only while in
   use (`sdProbeAtBoot` / `sdMount` / `sdUnmount`); leaving it mounted drops BLE to ~1 KB above its scan
   floor. `docs/DEVELOPER.md` §12.
12. Apple Silicon: Arduino's bundled ctags is x86_64; `tools/ctags/ctags` wraps universal-ctags. `build.sh`
   routes ctags through that wrapper **always**, so `brew install universal-ctags` is required on arm64 even
   when Rosetta is present (the wrapper exits if it cannot find it). arduino-cli caches prototype generation —
   `rm -rf build` after touching the wrapper.

## Where to change things
- Channel lists / dwell / scoring: top of `bandwatch.cpp` (`kChannels`, `kChanBand`, `kDwellMs`, `computeBusyScore`).
- Serial protocol: `sendHello/sendDwell/sendSweep/sendDevices/sendBleStatus`, `handleCommand`. Keep it in sync
  with `host/bandwatch_host.py` (`handle_line`, `merge_*`) and `docs/DEVELOPER.md`.
- LCD pages: `build*Page()` + `refresh*()`; add a page in the `Page` enum and `showPage()`.
- BLE: Bandwatch drives NimBLE `ble_gap_disc()` directly, **not** the Arduino `BLEScan` wrapper, because the
  wrapper merges advertisement payloads and cannot give per-packet data (`docs/DEVELOPER.md` §13). AD parsing
  is ours (`parseAdStructures`). Do not "simplify" this back to BLEScan. **`ble_addr_t.val` is
  little-endian**: reverse it for anything that displays or keys on a MAC, keep it raw for the pcap.
- **BLE mode emits no `{"t":"d"}` dwell lines**, so any live counter the dashboard needs must be added to
  `sendBleStatus()` as well as `sendDwell()`, or it will silently stop updating in BLE (`DEVELOPER.md` §14).
- Dashboard: `host/dashboard.html`, one `render*()` per section, polled from `/api/state` once a second. The
  poll is only 1 Hz, so anything the user clicks must latch a local pending state (see `armPending`) or it
  looks dead. `.capinfo` also carries `.control`, which is `display:grid` — override to block or inline
  content lands on separate rows.

## Testing without the LCD
Everything is observable over serial. From Python: open the port (DTR/RTS asserted), send `info`, read JSON
lines. Useful commands: `band 5g|2.4g|both|ble|154`, `park <ch>`, `cap 1/0`, `hunt <id> [ch]`, `deauth <bssid>|0` (Wi‑Fi modes only), `reboot`.
microSD: `sdcap 0|1` (record pcap on the card), `sdinfo`, `sdls`, `sdread <path>`, `time <epoch>` (no RTC —
the host sends this on connect; it dates the pcap records and names the files, in UTC).
Diagnostics for the deauth investigation (§11), not product features: `txtest 1|2|0` (inject a beacon with
SSID `BANDWATCH-TXTEST`; 2 = also disable promiscuous RX), `txstat` (TX counters), `softap <ch>|0`
(**proof of concept**: open SoftAP on that channel, injects from `WIFI_IF_AP`; tears down sniffing while up,
and its state handling is incomplete — do not build on it as-is).
Crash text is printed to USB before the reboot but the port re-enumerates, so keep a reader attached; decode
addresses with `riscv32-esp-elf-addr2line -pfiaC -e build/bandwatch.ino.elf <addr>`.

## Version history
v1.0 sweeps + LCD + dashboard + pcap · v1.1 BLE, device tables, hunt · v1.2 802.15.4 (Zigbee/Thread), pause/freeze tables · v1.2.2 deauth attack (spoof a BSSID, kick its stations) · v1.2.3 deauth crash fix + dead-man's-switch timeout + serial command-injection fix · v1.2.4 review pass: capture-ring leak on mode change, RF-string sanitising, host robustness · v1.2.5 deauth frame was a QoS-Null, not a deauth (fixed); attack still does not work - see docs/DEVELOPER.md section 11 · v1.3 microSD pcap recording (device writes the pcap; independent USB/SD sinks; sdls/sdread; on-demand mount) - section 12 · v1.4 BLE advertising capture as pcap link type 256, BLEScan replaced with direct NimBLE discovery - section 13 · v1.4.1 BLE MAC byte-order fix + auto scan policy · v1.4.2 capture telemetry in BLE mode, sdcap ack key, LCD record light.
