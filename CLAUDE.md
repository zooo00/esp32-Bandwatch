# Bandwatch — notes for an AI assistant (or any new developer)

Read `README.md` for what the product does and `docs/DEVELOPER.md` for how it is built. Open work is tracked in
`docs/BACKLOG.md` (bugs, verifications, decisions, features - each with a "done when"); design write-ups and history
live in `docs/ROADMAP.md`. This file is the short orientation.

## What this is
Firmware for the **Waveshare ESP32-C5-LCD-1.47** board (ESP32-C5, 1.47" ST7789 LCD, WS2812 LED, BOOT button)
plus a Python host tool. The device sniffs Wi-Fi (2.4 + 5 GHz), scans Bluetooth LE, sniffs IEEE 802.15.4
(Zigbee/Thread), or runs a 2.4 GHz energy-detect spectrum sweep (the `spec` mode: raw RF power per channel
via the 15.4 radio, flagging energy no decoder explained), shows results on the LCD, and streams JSON over
USB serial to `host/bandwatch_host.py`, which
serves a web dashboard on http://127.0.0.1:8080 and writes pcap files.

## Layout
- `bandwatch/` — Arduino sketch split into modules behind one shared header (`bandwatch_core.h`: tunables,
  channel model, and the state types + externs more than one file touches): `bandwatch.cpp` = core (channel
  hopping, dwell scoring, snapshots, hunt), `wifi_sniff.cpp` / `ble_scan.cpp` / `ieee154.cpp` = radio lifecycle
  + RX paths, `deauth_diag.cpp` = attack + diagnostics, `capture.cpp` = capture ring, `sd_sink.cpp` = microSD
  pcap + card presence, `events.cpp` = C4 event log to SD (`/events.csv`, `/seen.csv` baseline, `/surveil.csv`),
  `settings.cpp` = NVS persistence, `led_alert.cpp` = LED alert blips, `host_proto.cpp` = serial JSON, `lcd_ui.cpp` = LVGL pages. `devices.h` = device table structs/hash,
  `surv_ouis.h` = surveillance-OUI table. `Display_ST7789.*`, `LVGL_Driver.*`, `lv_conf.h` = display glue (from
  the upstream C6 project, pins changed); `boot_logos.h` = generated boot-photo arrays (`tools/img2c.py` from
  `bootlogo/`).
- `host/bandwatch_host.py` — serial reader, HTTP API (loopback by default; `POST /api/cmd` needs
  `Content-Type: application/json`, a same-origin `Origin` and a known `Host` - DEVELOPER §5), pcap writer, OUI/vendor lookup. `host/dashboard2.html` —
  the single-page UI at `/` and `/v2` (no build step, no dependencies). The classic `host/dashboard.html` was removed
  in v1.19 (git tag `v1.18.2` has the last copy); `/classic` 301-redirects to `/`, `--ui` is accepted and ignored.
- `build.sh` (compile/flash via arduino-cli), `setup.sh` (install toolchain), `tools/ctags/` (Apple-Silicon
  workaround), `tools/witness/` (RX-only ESP32-S3 monitor + `verify.py`, the on-air oracle for §11),
  `tools/deauth/patch_raw_tx.py` (post-build raw-TX patch), `docs/` (developer docs), `captures/` (pcaps,
  git-ignored).

## Hardware Debugging Checklist
Before blaming code or changing pins, clocks, or radio config, rule out the basics:
1. Confirm power: is the board on a powered port or hub?
2. Confirm the chip ID matches the bootloader (`esptool.py chip_id`). esptool resets the board (rule 3), so stop
   the host tool first and do this on purpose, not as a way to reboot.
3. Confirm the console UART routing (this board's console is the native USB-Serial-JTAG, `CDCOnBoot=cdc` in
   `build.sh`; there is no external UART bridge).
4. Erase NVS if a previous config change may have persisted (`settings.cpp` restores band mode, `addr1`,
   `blescan`, `specstep`, `snap`, `events` and `alerts` at boot).

Change one variable at a time. State the hypothesis before flashing.

## Build / flash / run (all from repo root)
```
./setup.sh                      # once per machine
./build.sh                      # compile
./build.sh --upload             # flash, raw-TX patched (stop the host tool first: it holds the serial port)
./build.sh --upload --no-patch  # flash the stock image (deauth then transmits nothing - rule 10)
python3 host/bandwatch_host.py  # dashboard + pcap
```
Serial console: 115200 baud, but open the port with **DTR and RTS asserted** (see below).

## New clone
Install the push guard once per machine: `cp tools/pre-push .git/hooks/pre-push && chmod +x .git/hooks/pre-push`
(see `AGENTS.md`).

## Hard-won rules for this board (do not relearn these)
1. **DTR/RTS edges reset the chip.** The C5's USB-Serial-JTAG maps DTR/RTS to BOOT/EN. Opening the port with
   pyserial `dtr=False, rts=False` reboots the board; `dtr=True, rts=True` (the macOS default on open) is safe.
   `arduino-cli monitor ... -c dtr=on,rts=on`. Never "toggle to reset" from the host.
2. **After flashing the chip may sit in ROM download mode** ("waiting for download", `boot:0x2e`) because a
   USB-initiated reset does not re-sample the BOOT strap. `build.sh` detects this and issues
   `esptool --before no-reset --after watchdog-reset run`; otherwise press RESET.
3. **Do not run esptool against a running board just to reset it** — its connection attempts toggle DTR/RTS
   repeatedly and reboot the board over and over.
4. **RAM (320 KB, no PSRAM) is the constraint.** Static usage must stay well under ~80 KB — an empirical
   line from 1.2, not a hardware limit; what actually binds is free heap at peak load (`DEVELOPER.md` §16).
   The build's "leaving 248104 bytes" is the linker's arithmetic, not reality: measured free heap at idle in `both`
   is ~99-106 kB depending on the LCD page (measured in v1.15.5, §16; static RAM has grown ~3.4 kB since). Only the
   visible LCD page exists as LVGL widgets; the capture ring is malloc'ed per capture; LVGL uses `LV_STDLIB_CLIB`;
   BLE keeps no result cache (we drive NimBLE directly), so BLE idles ~88 kB free (measured in the 2026-10 audit,
   v1.15.2). The LCD page swings free heap by ~7.5 kB (Overview heaviest; Channels, Devices and the Spectrum bars
   are custom-drawn); the capture ring reserves that swing (`lcdPageHeadroomB()`), so a new page must be built in
   `showPage()` like the others or its cost goes unseen. Out-of-memory shows up as
   `abort() ... lock_init_generic` or a store fault in `lv_obj_class_create_obj`.
   **`WifiDev` is field-ordered to pack to exactly 64 bytes** and a `static_assert` in `devices.h` holds it
   there: it lives in a `kWifiDevSlots` (96) array, so one added byte costs 96 and one added byte of *padding*
   can cost 384. `BleDev` (48 B) and `Dev154` (24 B) have the same kind of assert. If that assert fires, repack rather than raising the number.
5. **One radio.** Wi-Fi bands, BLE, 802.15.4 and the `spec` energy sweep are exclusive modes; `setBandMode()`
   tears one down and starts the next. `spec` reuses the 15.4 radio (and its channel 11-26 set) but arms no RX
   — it only runs `esp_ieee802154_energy_detect()` per channel, so it has no device table and no capture.
   Every mode change calls `releaseCapture()`: capture off *and* the ring (~32 KB in Wi-Fi, ~2.8 KB in BLE/15.4) returned to the heap
   (pcap link type differs per radio, and BLE mode needs that RAM). Only free the ring from the loop task.
6. **Serial output never blocks** (`setTxTimeoutMs(0)`, 8 KB TX buffer) and every line - acks and errors
   included - first checks `serialRoom()` (`Serial.availableForWrite()`) against a budget built from the real
   JSON-escaped string lengths, so lines are dropped whole, never truncated (v1.19.3). A budget that
   underestimates its line truncates mid-JSON instead, so keep budgets exact when adding fields.
7. **The prebuilt Arduino core cannot be reconfigured** (sdkconfig is fixed): BLE extended advertising is off,
   802.15.4 is on, 5 GHz Wi-Fi is on. Changing that means switching to ESP-IDF.
8. **Do not bump the core casually.** The deauth path pokes hard-coded offsets inside the prebuilt
   `libnet80211.a` (core 3.3.11 / IDF 5.5.5). Nothing checks them at runtime, so a different layout corrupts
   memory instead of failing. A `#warning` fires off 3.3.x; re-verify the offset table in `docs/DEVELOPER.md` §9.
9. **Strings off the air are hostile input.** SSID / BLE name / country code are control-character-stripped at
   ingest and JSON-escaped on the way out; keep both, or a crafted beacon corrupts a whole protocol line.
10. **The deauth attack works, but only on a patched image, and only the raw path** (1.6, measured with an
   external witness - `tools/witness/`; §11). Two separate faults were hiding each other: the raw
   `esp_wifi_80211_tx()` path was rejected by a subtype gate (`ESP_ERR_INVALID_ARG`), and the driver-internal
   slot path transmits **nothing for any subtype** - its §9 offsets are wrong - while reporting success. The
   raw path is now the default; `kickpath 1` selects the internal slot for offset work.
   `build.sh` applies `tools/deauth/patch_raw_tx.py` to the linked image by default (it patches out the gate
   and reseals the checksum/SHA-256, then restores the stock `.bin`); `./build.sh --upload --no-patch` flashes
   the stock image, whose deauth fails loudly with `ESP_ERR_INVALID_ARG`. The patch cannot live in the source
   tree - it edits the vendor blob inside the linked output, so every build re-applies it.
   **The counters still prove nothing** - `deauthSent`/`da` is incremented unconditionally and `ic_tx_pkt()`
   returns `void`; `da` once reached 328 while the witness heard zero. Confirm on air with
   `tools/witness/verify.py`, never from a counter. Whether a real station actually drops is still untested.
   Never hardcode the patch address: it moved from `0x420ff3b0` to `0x420ff436` just from adding one command.
11. **The SD card shares the LCD's SPI bus** (CS GPIO4, 20 MHz vs the LCD's 40 MHz). Safe only because both
   wrap transfers in beginTransaction/endTransaction and both run on the loop task — never touch the card
   from a radio callback or another task. Mounting FATFS costs ~30 KB, so the card is mounted only while in
   use (`sdProbeAtBoot` / `sdMount` / `sdUnmount`); leaving it mounted drops BLE to ~1 KB above its scan
   floor. `docs/DEVELOPER.md` §12.
   **Card presence changes only through `sdSetPresent()`** (it drives the LCD face, the `sd card removed|inserted`
   log line the host follows, and the event log's re-attach). There is no card-detect pin; the idle probe sends
   SPI CMD0, which **resets a card** - never send it (or call `sdProbeR1()`) while the card is mounted, recording or
   being read. Mounted users learn of a removal from their own I/O errors. `SD.end()` before every `SD.begin()`.
12. Apple Silicon: Arduino's bundled ctags is x86_64; `tools/ctags/ctags` wraps universal-ctags. `build.sh`
   routes ctags through that wrapper **always**, so `brew install universal-ctags` is required on arm64 even
   when Rosetta is present (the wrapper exits if it cannot find it). arduino-cli caches prototype generation —
   `rm -rf build` after touching the wrapper.

## Firmware Constraints

### RAM Safety
This firmware is RAM-constrained (rule 4, `docs/DEVELOPER.md` §16).
- After any firmware change, run `tests/run_offline.sh` (the firmware tier compiles and gates static RAM against
  `tests/firmware/static_ram_ceiling.json`) and report the static-RAM delta against the previous build. This is an
  arduino-cli build; `idf.py` is not used.
- Prefer variable-size buffers only with explicit bounds checks.
- Verify that serial TX buffers cannot overrun silently (rule 6). Detect and recover from drops at any point in a
  scan, not just at boundaries.

## Where to change things
- Surveillance OUIs: `kSurvOuis` in `surv_ouis.h` (firmware matches so the LCD can flag), names in
  `SURV_CAT`/`SURV_KIND` in the host. A match is evidence, not proof - keep the UI wording honest.
- Tier-1 (`addr1`) sightings must never evict or overwrite a device we heard transmit: see
  `trackWifiDevice(..., destOnly)` in `wifi_sniff.cpp` and `docs/DEVELOPER.md` §15.
- **Station-to-BSS association** is only the last 3 BSSID bytes on the device (`WifiDev.apSuffix`, set from the
  DS bits in `promiscuousCb` in `wifi_sniff.cpp`); the host completes the join against APs it has actually
  heard, and refuses to guess when two of them share those bits (`_resolve_parents`). `dca` is always given an
  AP's *full* BSSID, never the suffix. §17.
- Channel lists / dwell / scoring: `bandwatch_core.h` (`kChannels`, `kChanBand`, `kDwellMs`) +
  `computeBusyScore()` in `bandwatch.cpp`.
- Serial protocol (in `host_proto.cpp`): `sendHello/sendDwell/sendSweep/sendDevices/sendBleStatus`,
  `handleCommand`. Keep it in sync with `host/bandwatch_host.py` (`_dispatch`, `merge_*`) and
  `docs/DEVELOPER.md`.
- LCD pages: `build*Page()` + `refresh*()`; add a page in the `Page` enum and `showPage()`.
- Boot photos: sources in `bootlogo/`, regenerate `bandwatch/boot_logos.h` with `tools/img2c.py` (no PIL - sips plus
  a built-in PNG decode). Timings `kBootLogoMs` (boot, then the mode card) / `kWrapLogoMs` (linger + tap-step); the
  walk's photo stop is `walkPhoto` in `pollButton()` (lands after Spectrum), picture logic in `showLogoPic()`.
- BLE: Bandwatch drives NimBLE `ble_gap_disc()` directly, **not** the Arduino `BLEScan` wrapper, because the
  wrapper merges advertisement payloads and cannot give per-packet data (`docs/DEVELOPER.md` §13). AD parsing
  is ours (`parseAdStructures`). Do not "simplify" this back to BLEScan. **`ble_addr_t.val` is
  little-endian**: reverse it for anything that displays or keys on a MAC, keep it raw for the pcap.
- **BLE mode emits no `{"t":"d"}` dwell lines**, so any live counter the dashboard needs must be added to
  `sendBleStatus()` as well as `sendDwell()`, or it will silently stop updating in BLE (`DEVELOPER.md` §14).
- Dashboard: `host/dashboard2.html` is the only dashboard (at `/` and `/v2`; classic was removed in v1.19, tag
  `v1.18.2` has it). One `render*()` per section, polled from `/api/state` once a second. The
  poll is only 1 Hz, so anything the user clicks must latch a local pending state (see `armPending`) or it
  looks dead. `.capinfo` also carries `.control`, which is `display:grid` — override to block or inline
  content lands on separate rows.

## Testing without the LCD
Regression suite in `tests/` (`tests/README.md`): `tests/run_offline.sh` runs the host + firmware tiers (no board;
the firmware tier compiles and gates static RAM); `BANDWATCH_PORT=<port> python3 -m unittest discover -s tests/device`
runs the hardware tier against a live board (stop the host tool first).
Everything is observable over serial. From Python: open the port (DTR/RTS asserted), send `info`, read JSON
lines. Useful commands: `band 5g|2.4g|both|ble|154|spec`, `park <ch>`, `cap 1/0`, `hunt <id> [ch]`, `deauth <bssid>|0` (Wi‑Fi modes only, broadcast deauth), 
`dca <client_mac> <ap_bssid>` (targeted deauth to one station; both MACs must be colon-separated) | `dca 0`,
`snap <32..1600>` (capture snap length), `addr1 1|0` (also track Wi-Fi devices seen only as a frame destination,
tier 1, §15; persisted), `blescan active|passive|auto` (BLE scan policy; default `auto` stays
passive and opens a short active window when a new scannable device has no name), `specstep 1|2|5` (fine-spectrum
step in MHz; spec mode only), `mirror 1|0` (stream the LCD to the host over serial; default off, §19), `page next|prev` (step the LCD like a BOOT tap, §19), `reboot`.
microSD: `sdcap 0|1` (record pcap on the card), `sdinfo`, `sdls`, `sdread <path>`, `sdrm <name>` (delete one
card-root file; refused for the file being recorded, during an `sdread` or mid event-log flush; deleting `seen.csv`
while the event log is armed restarts novelty; §12), `time <epoch>` (no RTC —
the host sends this on connect; it dates the pcap records and names the files, in UTC), `events 1|0` (C4 event log
to `/events.csv`; persisted; arms with no card; status in `{"t":"ev"}` every 5 s, §20), `sdprobe` (raw CMD0 R1 of
the presence probe; -1 while the card is mounted/busy), `sdface 0|1` (show the card-out/card-in LCD face).
LED: `alerts 1|0` (alert blips on the LED - surveillance hit orange double, Zigbee permit-join purple double, new device
white single; default on, persisted, §21), `ledtest surv|new|join` (draw one blip now; never over a running deauth).
Diagnostics for the deauth investigation (§11), not product features: `txtest 1|2|0` (inject a beacon with
SSID `BANDWATCH-TXTEST`; 2 = also disable promiscuous RX), `txstat` (TX counters), `kickfc <hex>` (FC byte0 the internal kick path writes — `kickfc 80` sends a
beacon down the deauth descriptor path), `kickpath 0|1` (0 = raw `esp_wifi_80211_tx`, the
default and the only path that reaches the air; 1 = the dead internal slot, for §9 offset work), (`softap` was removed in the 1.6 review pass: its premise — that raw TX
radiates nothing from an unassociated STA — was disproven by the witness).
Crash text is printed to USB before the reboot but the port re-enumerates, so keep a reader attached; decode
addresses with `riscv32-esp-elf-addr2line -pfiaC -e build/bandwatch.ino.elf <addr>`.

## Git & Releases

### Release Workflow
A release means all of these steps, in order:
1. Bump the version: `kVersion` in `bandwatch/bandwatch_core.h` is the only version string (the host and the
   dashboard show the device's `hello.ver`).
2. Update the README (if anything user-visible changed) and add the `CHANGELOG.md` entry, so the tag points at
   current docs.
3. Build and flash, then smoke-test on the device.
4. Commit.
5. Create the tag `vX.Y.Z` and verify it points at the right commit (`git show vX.Y.Z --stat`).
6. Push the branch AND the tags.

Never leave a commit unpushed without explicitly telling the user. Force-pushes and retags must be handed to the
user, with the exact command to run.

## Preferences

### Working Style
- When I say 'plan and build', give a short plan (bullets) and start implementing. Skip extended opinion unless I ask.
- For visual and dashboard changes, describe the expected look and keep the existing encoding (e.g., waterfall =
  energy heatmap) unless told otherwise.
- In zsh, quote globs like `/dev/cu.*` or use `ls /dev/cu.* 2>/dev/null`.

## Version history
See `CHANGELOG.md` (newest first). Add an entry there for every release; keep this file for orientation only.
