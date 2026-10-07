# Bandwatch — notes for an AI assistant (or any new developer)

Read `README.md` for what the product does and `docs/DEVELOPER.md` for how it is built. Planned work and
open questions live in `docs/ROADMAP.md`. This file is the short orientation.

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
  pcap, `host_proto.cpp` = serial JSON, `lcd_ui.cpp` = LVGL pages. `devices.h` = device table structs/hash,
  `surv_ouis.h` = surveillance-OUI table. `Display_ST7789.*`, `LVGL_Driver.*`, `lv_conf.h` = display glue (from
  the upstream C6 project, pins changed); `boot_logos.h` = generated boot-photo arrays (`tools/img2c.py` from
  `bootlogo/`).
- `host/bandwatch_host.py` — serial reader, HTTP API, pcap writer, OUI/vendor lookup. `host/dashboard.html` —
  the single-page UI (no build step, no dependencies).
- `build.sh` (compile/flash via arduino-cli), `setup.sh` (install toolchain), `tools/ctags/` (Apple-Silicon
  workaround), `tools/witness/` (RX-only ESP32-S3 monitor + `verify.py`, the on-air oracle for §11),
  `tools/deauth/patch_raw_tx.py` (post-build raw-TX patch), `docs/` (developer docs), `captures/` (pcaps,
  git-ignored).

## Build / flash / run (all from repo root)
```
./setup.sh                      # once per machine
./build.sh                      # compile
./build.sh --upload             # flash, raw-TX patched (stop the host tool first: it holds the serial port)
./build.sh --upload --no-patch  # flash the stock image (deauth then transmits nothing - rule 10)
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
4. **RAM (320 KB, no PSRAM) is the constraint.** Static usage must stay well under ~80 KB — an empirical
   line from 1.2, not a hardware limit; what actually binds is free heap at peak load (`DEVELOPER.md` §16).
   The build's "leaving 248104 bytes" is the linker's arithmetic, not reality: measured free heap is ~83 kB. Only the visible LCD
   page exists as LVGL widgets; the capture ring is malloc'ed per capture; LVGL uses `LV_STDLIB_CLIB`; the BLE
   BLE keeps no result cache (we drive NimBLE directly), so BLE idles ~88 kB free. The LCD page swings free heap
   by ~20 kB (Overview heaviest, Channels lightest since it is custom-drawn); the capture ring reserves that
   swing (`lcdPageHeadroomB()`), so a new page must be built in `showPage()` like the others or its cost goes unseen. Out-of-memory shows up as
   `abort() ... lock_init_generic` or a store fault in `lv_obj_class_create_obj`.
   **`WifiDev` is field-ordered to pack to exactly 64 bytes** and a `static_assert` in `devices.h` holds it
   there: it lives in a `kWifiDevSlots` (64) array, so one added byte costs 64 and one added byte of *padding*
   can cost 256. If that assert fires, repack rather than raising the number.
5. **One radio.** Wi-Fi bands, BLE, 802.15.4 and the `spec` energy sweep are exclusive modes; `setBandMode()`
   tears one down and starts the next. `spec` reuses the 15.4 radio (and its channel 11-26 set) but arms no RX
   — it only runs `esp_ieee802154_energy_detect()` per channel, so it has no device table and no capture.
   Every mode change calls `releaseCapture()`: capture off *and* the ~32 KB ring returned to the heap
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
12. Apple Silicon: Arduino's bundled ctags is x86_64; `tools/ctags/ctags` wraps universal-ctags. `build.sh`
   routes ctags through that wrapper **always**, so `brew install universal-ctags` is required on arm64 even
   when Rosetta is present (the wrapper exits if it cannot find it). arduino-cli caches prototype generation —
   `rm -rf build` after touching the wrapper.

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
- Dashboard: `host/dashboard.html`, one `render*()` per section, polled from `/api/state` once a second. The
  poll is only 1 Hz, so anything the user clicks must latch a local pending state (see `armPending`) or it
  looks dead. `.capinfo` also carries `.control`, which is `display:grid` — override to block or inline
  content lands on separate rows.

## Testing without the LCD
Everything is observable over serial. From Python: open the port (DTR/RTS asserted), send `info`, read JSON
lines. Useful commands: `band 5g|2.4g|both|ble|154|spec`, `park <ch>`, `cap 1/0`, `hunt <id> [ch]`, `deauth <bssid>|0` (Wi‑Fi modes only, broadcast deauth), 
`dca <client_mac> <ap_bssid>` (targeted deauth to one station; both MACs must be colon-separated) | `dca 0`,
`snap <32..1600>` (capture snap length), `blescan active|passive|auto` (BLE scan policy; default `auto` stays
passive and opens a short active window when a new scannable device has no name), `specstep 1|2|5` (fine-spectrum
step in MHz; spec mode only), `mirror 1|0` (stream the LCD to the host over serial; default off, §19), `page next|prev` (step the LCD like a BOOT tap, §19), `reboot`.
microSD: `sdcap 0|1` (record pcap on the card), `sdinfo`, `sdls`, `sdread <path>`, `time <epoch>` (no RTC —
the host sends this on connect; it dates the pcap records and names the files, in UTC).
Diagnostics for the deauth investigation (§11), not product features: `txtest 1|2|0` (inject a beacon with
SSID `BANDWATCH-TXTEST`; 2 = also disable promiscuous RX), `txstat` (TX counters), `kickfc <hex>` (FC byte0 the internal kick path writes — `kickfc 80` sends a
beacon down the deauth descriptor path), `kickpath 0|1` (0 = raw `esp_wifi_80211_tx`, the
default and the only path that reaches the air; 1 = the dead internal slot, for §9 offset work), (`softap` was removed in the 1.6 review pass: its premise — that raw TX
radiates nothing from an unassociated STA — was disproven by the witness).
Crash text is printed to USB before the reboot but the port re-enumerates, so keep a reader attached; decode
addresses with `riscv32-esp-elf-addr2line -pfiaC -e build/bandwatch.ino.elf <addr>`.

## Version history
v1.0 sweeps + LCD + dashboard + pcap · v1.1 BLE, device tables, hunt · v1.2 802.15.4 (Zigbee/Thread), pause/freeze tables · v1.2.2 deauth attack (spoof a BSSID, kick its stations) · v1.2.3 deauth crash fix + dead-man's-switch timeout + serial command-injection fix · v1.2.4 review pass: capture-ring leak on mode change, RF-string sanitising, host robustness · v1.2.5 deauth frame was a QoS-Null, not a deauth (fixed); attack still does not work - see docs/DEVELOPER.md section 11 · v1.3 microSD pcap recording (device writes the pcap; independent USB/SD sinks; sdls/sdread; on-demand mount) - section 12 · v1.4 BLE advertising capture as pcap link type 256, BLEScan replaced with direct NimBLE discovery - section 13 · v1.4.1 BLE MAC byte-order fix + auto scan policy · v1.4.2 capture telemetry in BLE mode, sdcap ack key, LCD record light · v1.5 surveillance OUI flagging, addr1 tier-1 sightings, non-blocking sdread - section 15 · v1.5.1 review pass: firmware split into modules behind `bandwatch_core.h`, capture refused below a 24 kB free-heap floor, `sdls` reports total/sent (host shows the card's file list) - sections 3 and 16 · v1.5.2 deauth sequence numbers fixed (all frames in burst now have unique seq; previously identical 0x00), diagnostic logging added for TX path verification - attack still does not work, root cause unidentified (see DEVELOPER.md §11) · v1.5.3 targeted deauth (`dca`) · v1.5.4 the 1.5.3 `dca` UI was unreachable end-to-end (four independent breaks); station-to-BSS association added to make it work, at zero static-RAM cost - section 17 · v1.5.5 merge of the two 1.5.x lines: 1.5.3/1.5.4 were built on the pre-split monolith, so `dca` and the association tracking were ported onto the module layout (1.5.3/1.5.4 firmware tags predate the split and do not build from this tree). · v1.6 the deauth attack transmits: an external witness (`tools/witness/`, a second ESP32 in monitor mode) proved neither path reached the air, for two unrelated reasons - the raw path was rejected by libnet80211's subtype gate, and the internal slot path radiates nothing for *any* subtype, so the section 9 offsets are wrong rather than merely fragile. Raw TX is now the default and `tools/deauth/patch_raw_tx.py` patches out the gate post-build; 289 deauth frames witnessed on air at -38 dBm - section 11. · v1.7 spectrum analyzer: a `spec` mode runs the 15.4 radio's energy-detect primitive across channels 11-26 for a true 2.4 GHz RF-power reading (dBm, no decode) - the only real noise-floor measurement this board exposes; 5 GHz has no equivalent. The host correlates per-bin energy against recently-decoded Wi-Fi/BLE/Zigbee to flag *unexplained* energy (an emitter with an unknown/undecodable protocol - evidence, not an ID; the radio cannot demodulate it). New LCD spectrum page + dashboard spectrum tab (bars, waterfall, unexplained-energy list) - section 18. · v1.7.1 fix: the 1.7 "faster scan" hopped channels every 60 ms from the loop, but the 15.4 radio needs ~>100 ms to settle after set_channel, so energy detect returned a stuck ~-40 dBm on every channel (flat, not tracking real occupancy). Reverted to the 120 ms UI-timer hop (~2 s sweep); floor back to ~-110 dBm with real structure. Also: combined 2.4-energy + 5-GHz-activity dashboard view, auto-ranged dBm axis with peak-hold, Wi-Fi/BLE activity bars, responsive-layout fix. · v1.7.2 review pass (carries all 1.7.1 energy fixes): RF-energy bar palette no longer starts at near-black (vanished on dark grey); Overview chart/tooltip no longer mislabel spec's 15.4 bins as 5 GHz; hello/system page report dwellMs() not the fixed kDwellMs; host guards the energy24/activity5/spec_hist caches with dlock (were mutated on the reader thread while HTTP threads read them); hopActive() helper. · v1.7.3 energy now tracks the Wi-Fi band: the 128 us ED window listened only ~5% of each dwell and missed bursty Wi-Fi (a strong but idle AP showed no energy); widened kEdDurationSym to 2 ms (~50% coverage) so energy peaks line up with the actual APs. · v1.7.4 dashboard: warn when every 2.4 GHz bin's noise floor is elevated (> -85 dBm; healthy ~-110) - a strong signal desensitising the 15.4 receiver, or a stuck radio state - instead of showing a misleading flat spectrum. Dashboard-only (kVersion bumped for release parity). · v1.7.5 dashboard refactor (review nit): factor the four bar charts' shared grid/axis + tooltip scaffolding into gridAxis()/chartTips(); no behavior change. Dashboard-only (kVersion bumped for parity). · v1.8 fine spectrum: spec mode now sweeps 2400-2483 MHz in a configurable 1/2/5 MHz step (default 2) by tuning the 15.4 synth off the channel grid via `ieee802154_ll_set_freq()` (below the public channel API, but an inline HAL function - cleaner than the deauth blob patch; off-grid reads verified against known APs). ~42 bins at 2 MHz vs the old 16 channels, full-band incl. edges; still 2.4 GHz only (no 5 GHz energy). Freq-based serial messages `fs` (full sweep) / `fd` (per-dwell cursor), `specstep` command, dashboard resolution toggles, LCD maps the bins onto 42 fixed bars. Section 18. · v1.9 dashboard v2 + host sdread reassembly + firmware review pass: a fresh left-rail dashboard (`host/dashboard2.html`, no tabs - controls grouped in a sticky rail, one scrolling main column) runs *in parallel* with the classic one, both always served (`--ui v2` or `host/run-v2.sh` puts v2 at `/`, the other at `/classic` or `/v2`, cross-linked). The host now reassembles the `S <n> <base64>` file chunks the device has streamed since v1.3 but nothing ever collected (they fell through to the JSON parser and flooded the log pane): `handle_sd_chunk` buffers them, `sdread_done` writes the file into the captures dir, `GET /file?name=` serves it for download, `POST sdread <path>` validates against the card naming scheme. Firmware review: `sendInternalKick()` seq encoding now matches `sendKickFrame()` (seq<<4 low/high bytes, clear fragment nibble - the old code put the seq low-nibble in the fragment field and repeated every 256 frames); the dead spec branch in `finishDwell()` dropped (v1.8's early return into `specFine[]` made it unreachable); `noteHuntHit()` on the Wi-Fi/BLE paths checks `hunt.kind == 0`, symmetric with 15.4's `kind == 1`, so a 15.4 key hunt no longer matches a stale MAC; `cap`/`sdcap` refuse in spec mode (no RX armed) instead of recording into a file that never grows. · v1.10 LCD mode splash + layout pass: a band change (button hold-walk, host `band` command, or boot) flashes a full-screen name card for the new mode before its scan page takes over; holding BOOT walks the six cards at a 700 ms cadence and release commits the one on screen (hunt page stops the hunt first, then walks). Every widget width re-checked against the Montserrat glyph tables (header right labels to font 12 so `park 165 USB` fits, overview title `Bandwatch`->`Activity`, Top-3 and channels-grid columns widened/clipped, dual-band header reads `BOTH chNN`). Host: `sdread` name guard tightened to 39 chars - `sdread /<name>` is 8 + len chars and the device line buffer holds 47, so a 40-char name lost its last char. · v1.11 roadmap batch one (C3+C6+C8), measured +16 B static total (80,544 -> 80,560), flashed and verified on-device: **C3** NVS settings persistence (`settings.cpp` - band mode + addr1/blescan/specstep/snap survive a power-cycle; park and hunt/deauth deliberately not persisted so a reboot stops transmitting and never boots silently parked; transient RAM only, the `Preferences` handle closes after each load/save); **C6** top talker per dwell (`Accum.bestMac`/`bestRssi`, one instance read by `sendDwell` before `resetAccum`, so `"top"`/`"trssi"` on the `d` line cost no per-channel RAM; serial budget 380->420; both dashboards show a Top-talker column joined to the device table); **C8** least-busy readout (`quietestChannel()`, LCD Overview footer + a Quietest-channel tile on both dashboards; UI-only). Then a RAM-reclaim pass (same release) cut static from 80,560 -> **74,480 B** (-6,080, back under the old ~80 kB line, ~6 kB more free heap - boot "heap after wifi" ~83 -> ~88 kB): **(1)** LVGL draw buffer `buf1` divisor `W*H/14` -> `/21` (7,862 -> 5,241 B; PARTIAL render just paints more chunks, invisible for this UI - raise back toward /14 if the mode splash flickers); **(2)** the device-table listing path no longer copies whole tables into a 4 kB `DevSnap` union - `collect*Refs()` copies a compact `DevRef{rssi,idx,key[8]}` per fresh device under the lock and sorts those (one `g_devRefs[64]` = 640 B), then `fetch*Dev()` re-validates each slot and copies out one full record; a slot evicted/reused between collect and fetch fails the identity check and is skipped that cycle (saves ~3.3 kB). The static-RAM gate for future work: free heap at peak binds, not the static number - the tightest peak is SD capture (was ~31.9 kB free vs the 24 kB `kMinFreeHeapB` refuse-floor; now ~6 kB higher). Remaining reclaim levers: device-table slot counts (`kWifiDevSlots` 64->48 ~2 kB, costs tracking capacity), or the ESP-IDF port for the ~59 kB Wi-Fi/BLE stack that is otherwise fixed. · v1.12 boot photos: eight pictures from `bootlogo/` embedded as RGB565 in flash (`bandwatch/boot_logos.h`, generated by `tools/img2c.py`; ~860 KB of flash, no RAM cost - LVGL draws the const array directly). A random one shows full-screen for 2 s at boot before the mode card; the BOOT hold-walk gains a photo stop after Spectrum (the last mode) - release there and it lingers 5 s while taps step through the pictures in order; tapping past the last page flashes one on the way around. Timings: `kBootLogoMs` / `kWrapLogoMs`. · v1.12.1 public-release housekeeping + a boot-splash fix: MIT `LICENSE` + `NOTICE` (MIT covers only this project's work; the upstream PierreGode/WaveshareESP32C6LCD base ships no license of its own, so its derived portions are carved out, not relicensed) and a README Credits section for the C6->C5 lineage; `build.sh` auto-regenerates `bandwatch/boot_logos.h` from `bootlogo/boot_*.png` when they change (missing header, newer PNG, or count != `K_BOOT_LOGO_COUNT`; `LOGOS=always`/`never` override). Fix: `showBandSplash()` now hides the boot-photo overlay (and clears `logoThenModeCard`), so tapping BOOT during a committed-mode splash - e.g. after releasing the hold-walk on Spectrum, before it hands off to the scan page - no longer drops into the photo loop; it used to draw the mode card over a still-"active" logo and `logoActive()` only tested the hidden flag. · v1.12.2 boot-photo UX: removed the page-wrap picture flash (`showLogoSplash` on wrap in `pollButton()`) - tapping through pages no longer flashes a random picture when it loops from the last page back to the first, and so no longer briefly captures taps into picture-stepping; pictures now appear only at boot and on the hold-walk photo stop after Spectrum. · v1.13 live LCD mirror: `mirror 1|0` streams the 172x320 screen to the host over USB serial (default off, not persisted). The LVGL flush callback (`mirrorOnFlush` in host_proto.cpp, hooked from `Lvgl_Display_LCD`) emits each repainted region as an `M x y w h <base64 RGB565-LE>` line; `serviceMirror()` (lcd_ui.cpp, called from the loop) paces a full re-send one ~10-row strip per loop so a bulk repaint never overruns the 8 KB TX buffer; no extra device RAM (piggybacks the existing partial draw buffer). Host `handle_mirror` reassembles a framebuffer, serves `/screen.bin` + `/api/screen`; the v2 dashboard paints it on a canvas (Start-mirror card). Section 19. · v1.13.1 mirror fix: a strip dropped during the initial scan (collides with a data-refresh flush, overruns the TX buffer) left a permanent black/stale band because mid-scan drops were ignored; `mirrorNoteDrop()` now flags a re-scan that repeats until a pass completes with no drop (self-heals). · v1.14 mirror UI: the LCD-mirror panel moved into the dashboard's left control rail (172x320 canvas fits its width); added `page next|prev` (`stepPage()` in lcd_ui.cpp, BOOT-tap emulation) with **Page ‹ ›** buttons so the interface can be stepped from the browser - the practical answer to fast-updating screens (spectrum) tearing over the serial link. · v1.15 review pass: spectrum 2.4 GHz energy updates per probe in the dashboard (host `fd` handler writes each per-dwell bin live, not only the full-sweep `fs`; other modes already update per-dwell via the `d` line); BLE dashboard chart replaced - per-device RSSI bars (redundant with the table) became a "What's advertising" category breakdown (Find My/AirTag, AirPods/audio, phone/Mac/Watch, beacon, other); LCD 802.15.4 Overview/Channels footer reads "nodes <n>" (live `collect154Refs` count) instead of a Wi-Fi-only "APs 0". · v1.15.1 dashboard: the Wi-Fi "max busy over time" trend read as a solid blob (it plots the busiest channel, which sits high, under a flat 55%-opacity area fill); switched to a fade-out gradient fill (`#trendGrad` in `renderTrend`) so the line/dips read clearly, retitled "Busiest channel over time" with a plain-language `<small>` explainer. Dashboard-only (kVersion bumped for parity). · v1.15.2 trend gained a second series: band average (mean of channels sampled in the last ~15 s, stored as history tuple element 4 in the `d` handler) drawn as a dashed line under the solid busiest line, with a legend; retitled "Channel load over time". The max pins high in busy air, the average moves. Host+dashboard (kVersion bumped for parity). · v1.15.3 memory hygiene (from the 2026-10 RAM audit, measured on-device): **`cap 1` then `sdcap 1` defeated the 24 kB heap floor** - the ring was floor-checked before the FATFS mount, landing at 16.1 kB free; `sdcap` now calls `refitCapRing()`, which re-sizes an existing ring against post-mount heap without touching either sink (20 -> 11 slots, 30.5 kB), and `ensureCapRing()` now *sizes* the ring to leave the floor standing instead of refusing outright. **Park in spec mode is honoured** (it holds the fine sweep on the parked 15.4 channel's centre bin; full sweeps/`fs` pause while parked) and v2's park control is enabled in spec with a truthful note. BLE device-row serial budget 102 -> 116 (a maxed row is ~110 B; under-budget truncates the line mid-write, rule 6); Wi-Fi stays at 126 because 64 x 126 + 40 is the most the 8 KB TX buffer allows. Dropped the upstream empty-touchpad LVGL indev. LCD page heap swing (~45 kB, Channels the heaviest) documented in §16; the Channels-page redraw is the next RAM lever (ROADMAP). · v1.15.4 Channels LCD page custom-drawn: its 39 rows x (row + 2 labels + bar), ~156 LVGL objects, became one object painted in `chanGridDraw()` from a 3-byte `ChanCell` snapshot, invalidating only changed cells - free heap on that page 56.1 -> **106.3 kB** (+50 kB), static 74,504 -> 74,032 B; it is now the lightest page and Overview (~87.6 kB) the heaviest. That inverted a risk: a capture ring sized on a light page went under the 24 kB floor on the next step to Overview (measured 16.4 kB), so `showPage()` now records each page's build cost and the ring also leaves `lcdPageHeadroomB()` (step to the heaviest page available in the mode) - USB+SD capture now bottoms out at ~30.8 kB from any starting page. Docs: ROADMAP staleness pass (release, static/heap numbers, serial budgets, WifiDev now one array, C9's EAPOL test was the Authentication subtype, 1.6.2/1.6.4 folded into C4, 1.13 mirror section); `tools/probe_pages.py` names pages from the device's ack. · v1.15.5 Overview's per-channel strip and the Devices list custom-drawn too (`ovStripDraw`, `devListDraw`; Devices repaints only rows whose FNV fingerprint changed): Overview 87.6 -> 98.9 kB, Devices 91.2 -> 106.4 kB free, pages now span ~99-106 kB; static 74,032 -> 73,904 B. Drawn one-line text needs `LV_TEXT_FLAG_EXPAND` (`lv_draw_label` word-wraps in its box; LONG_CLIP did not). Fixes found on the way: tier-1 (`~`, destination-only) Wi-Fi devices carry rssi 0 and sorted *above* every real transmitter, pushing the loudest real devices off the LCD's 12 rows - `collectWifiRefs()` now sorts them last, and the LCD shows `--` with an empty bar instead of "0" with a full red one.
