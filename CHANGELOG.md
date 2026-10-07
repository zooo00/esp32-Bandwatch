# Changelog

Newest first. Moved here from `CLAUDE.md` (which now only points at this file). Details live in `docs/DEVELOPER.md`
and `docs/ROADMAP.md`; open work is in `docs/BACKLOG.md`.

## v1.19.4

Dashboard layout, no protocol or firmware behaviour change (`kVersion` only).

- In BLE and Spectrum mode the last-known channel views (busy score by channel, channel load over time,
networks being sought, the Channels table) move into one collapsed "Channel views" group below the live content,
with the staleness in its summary line; the hopping modes keep them in place, unchanged. Previously they sat above the
live BLE chart and pushed it below the fold.
- Wi-Fi table: a device seen only as a frame destination shows "–" for its channel instead of "0".
- The deauth station picker's ⚙ is now a normal small button (it was a borderless 11.5 px glyph) and stays on the
same line as Deauth.

## v1.19.3

Combined review pass: fixes from four independent code reviews, merged into one plan. Compiled and offline-tested
(host 111, firmware 3), and the device tier passed 26/26 on the board (`tests/device/RESULTS.md`).

- **Security (host + dashboard).**
  - Dashboard XSS closed: a crafted SSID, security string or vendor in the deauth bar ran script on the dashboard
    origin. Every off-air value is now escaped, and `esc()` also escapes `'`.
  - `POST /api/cmd` now requires `Content-Type: application/json` and a same-origin `Origin`. `Host` is validated on
    every request (DNS-rebinding guard).
  - Every response carries anti-framing headers (`X-Frame-Options`, CSP `frame-ancestors`) and `nosniff`.
  - A non-loopback `--bind` prints a warning.
  - Request bodies: `Content-Length` must be numeric (400) and at most 64 KB (413), and a JSON body must be an object.
  - Errors are non-2xx `{"ok":false,"error"}`. Device commands get 503 when no board is connected, and no empty pcap is
    created.
- **Dashboard.**
  - BACKLOG B1: the hunt and deauth bars show at the same time, each with its own Stop.
  - BACKLOG B2: the deauth row buttons no longer stick on "…" after an attack stops.
  - Commands: refused commands toast their error, and the state poll has a timeout.
  - Tables: sort/filter work while hovering a table or while paused.
  - Keyboard: Space no longer pauses when a control has focus.
  - Layout: the deauth client picker is no longer clipped, and the black `.hit` columns are gone from the charts.
  - Dest-only Wi-Fi rows show "–" instead of 0 dBm.
  - Wording: "now ago" now reads "just now"; toast colours, `aria-live`, keyboard device-card headers, one "Wi-Fi"
    spelling.
- **Host.**
  - Concurrency: one state lock with `snapshot()` copying under it, which fixes "deque mutated during iteration".
    Capture start/stop is race-free, same-second captures get `-N` names, and `snaplen` is validated before the file
    opens.
  - Serial: writes time out after 1 s, and the line buffer is bounded.
  - Bounds: device tables are capped at 1024 entries, and probe SSIDs at 32 per MAC.
  - `sdread`: a pull is checked against its size, so a short one is reported failed and not saved.
  - Spectrum: `spec_hist` clears on a step change. `explain` always clears its active flag.
  - Files: `/file` streams and only serves safe `.pcap`/`.csv` names.
  - pcap: the writer flushes every second, and SIGTERM closes it cleanly.
  - Dest-only Wi-Fi rows report `rssi`/`max` as null.
- **Firmware.**
  - Rule 6 now holds for every line, acks and errors included. `sendLinef()` and `jsonStrLen()` replace the estimated
    budgets, Wi-Fi and BLE row budgets use the real escaped SSID/name lengths, and the sweep, z, spectrum-bin and hello
    budgets are derived worst cases. `sdread_done` waits for room instead of being lost.
  - Hunt: the 8-byte 802.15.4 extended-address hunt works. `parseMac` accepted it as a 6-byte MAC; it now rejects
    trailing characters. A key hunt only parks in 15.4 mode.
  - `age_ms` no longer underflows to ~4.29e9 for a device heard during a snapshot.
  - Deauth:
    - Serial output moved out of the `g_devMux` critical sections.
    - The default raw path no longer reads the hard-coded `libnet80211` offsets; only `kickpath 1` latches the
      internal slot.
    - TX power: values are in 0.25 dBm units. 82 = 20.5 dBm, not the "8.2 dBm" the comments claimed. The attack now
      sets 84 (the maximum) instead of an out-of-range 160.
  - Capture ring slots are sized per radio: 128 B in BLE/15.4 instead of 1600 B, about 2.8 KB of heap instead of
    about 32 KB (unmeasured on the board).
  - SD:
    - `sdread` paths and `sdls` names are JSON-escaped.
    - The pcap name search fails instead of truncating `-9999`.
    - The sdread failure path uses `sdUnmount()` (it no longer unmounts under a running `sdcap`).
  - Smaller fixes:
    - The `time` ack's `ok` means "applied".
    - `/surveil.csv` lines must end cleanly.
    - Fresh tier-1 slots no longer seed `maxRssi` from the sender.
    - The logo timer is wrap-safe.
  - Dead code removed (`ChannelState.ed*`, `Deauth.dumped`, `probeDropped`, `sd.dropped`). Static RAM 77,392 →
    **77,184 B**; the ceiling is restamped to 77,696 B.
- **Docs.**
  - `CLAUDE.md`: the leaked instruction fragments are now real sections.
  - README, DEVELOPER, BACKLOG, ROADMAP, RAM-AUDIT, WHY-DEAUTH and the tests README are aligned with the code: modes,
    pages, HTTP API, budgets, TX power units, RAM figures, and the 3 MB app partition.

## v1.19.2

Docs and housekeeping: `CHANGELOG.md` created (history moved out of `CLAUDE.md`), `kVersion` brought in line with the release (it still said 1.19 at v1.19.1), stale notes fixed (RAM-check wording, static-RAM figure, classic-dashboard wording, backlog I1). No behaviour change.

## v1.19.1

host/dashboard: the SD card file list moved out of the narrow rail into a full-width "SD card files" card, grouped (Wi-Fi / BLE / 802.15.4 captures, event log, device baseline, surveillance list, other) with full file names, capture time parsed from the name (UTC), size, per-group "show all", Download and two-click Delete; the rail links to it. The host opens the serial port **exclusively** - two hosts on one port used to split the bytes ("readiness to read but returned no data", reconnects every few seconds, lost lines - it looked like a hung board, likely BACKLOG B4); a second one is now refused with a clear message. A reconnect's hello no longer blanks the card's file list while the card stays mounted.

## v1.19

backlog decisions D1/D2/D4: **LED alert blips** (`led_alert.cpp`, DEVELOPER §21) - an alert wins the LED for 300-500 ms then yields back (orange double = surveillance hit, first per device per session, independent of the event log; purple double = 802.15.4 permit-join first seen; white single = C4 `new` row); never over an active deauth (dropped, not queued); at most one blip per 2 s, highest kind wins; `alerts 1|0` (NVS, default on), `ledtest surv|new|join`; +104 B static. **Classic dashboard retired** (`host/dashboard.html`, `run-v2.sh` removed; `/classic` 301 -> `/`; `--ui` accepted and ignored; last copy at tag v1.18.2). **Card files**: `/seen.csv` rotates when an attach finds > 4096 entries (old -> `/seen.old.csv`, newest 2048 rewritten, baseline unchanged, two-pass copy with no MAC buffer, skipped below 24 kB free heap); `sdrm <name>` deletes one card-root file (plain names only; refused for the file being recorded, during an sdread or mid-flush; deleting `seen.csv` while armed restarts novelty); dashboard v2 lists every card file with Download and a two-click Delete. `tools/power_profile.py` walks the modes for an inline USB power meter and prints battery life. Tests: host 85, firmware 3 (77,392 B), device suite + `T11SdRm`/`T12Alerts`.

## v1.18.2

regression suite + dashboard gaps: `tests/` in three tiers - host (71 offline tests: protocol, probes, events, SD, mirror framing, HTTP routing), firmware (compile + static-RAM ceiling 77,800 B + ASCII LCD strings), device (19 on-board tests incl. a per-mode no-truncated-lines soak; all pass). The suite found and fixed four host bugs: a non-object JSON line ended the serial session (and a running capture); one malformed device row broke `/api/state` until restart; `sdread`'s pcap pattern allowed `/`; the mirror status's "complete" flag was overwritten by a counter of the same name. Firmware: `page prev` skipped unavailable pages forward and got stuck - `showPage(n, dir)` now skips in the step direction. Dashboard v2 closed five of six classic gaps (cached spectrum + unexplained count in every mode, probe groups and channel views kept as last-known, BLE filter by company); hunt+deauth side by side is open (docs/BACKLOG.md B1). New `docs/BACKLOG.md`: the single list of open work.

## v1.18.1

LCD mirror frames: the flush callback passes `lv_display_flush_is_last()` and `mirrorOnFlush` ends each LVGL refresh with `MF <seq> <complete>`; regions go out as <= ~2 KB row slices (a ~7 KB flush chunk as one line only fit a nearly empty TX buffer, so busy pages dropped almost everything); a dropped slice joins one repair rectangle that `serviceMirror()` re-sends in TX-sized strips (replacing 1.13.1's whole-screen re-scan). The host blits into a back buffer and publishes at markers (complete frames at once, torn ones after 0.5 s; legacy firmware per region). Measured: ~57% complete frames on Wi-Fi pages; 0% on the BLE Devices page, which repaints ~220 KB/s of base64 - beyond the link, shown as "repairing" (§19).

## v1.18

**C4 event log to SD** (`events.cpp`, DEVELOPER §20; also delivers ROADMAP 1.6.2 + 1.6.4): `events 1|0`, persisted in NVS (`events` key), arms even with no card. `/events.csv` (`epoch_ms,up_ms,kind,id,rssi,ch,radio,extra`) gets `surv` rows (surveillance-OUI match, first per device per session, `extra` = category + " (dest only)" for tier 1) and `new` rows (a globally unique MAC not in the card's `/seen.csv` baseline; Wi-Fi locally-administered and BLE random addresses never count as new). Baseline = newest 2048 `/seen.csv` entries as sorted 32-bit FNV hashes (`kBaseCap` 2560), grown by appending each flush's new MACs. Radio paths `eventFlag()` under `g_devMux` on slot creation into a 16-entry queue; `serviceEvents()` (loop) buffers rows in 2 KB of heap and mounts the card only to flush (60 s / 16 rows / 48 pending MACs; waits while `sdcap` or `sdread` owns the card); rotation at 1 MB to `/events.old.csv`. No card: novelty suspended (`wait`), surveillance rows buffer, mount retried every 30 s or on insertion; a failed mount/write marks the card lost and the next mount reloads that card's baseline and re-inserts the session's pending new MACs. `ev` object on hello, the `events` ack and `{"t":"ev"}` every 5 s while armed. `/surveil.csv` = up to 64 extra OUIs read at boot (built-in table first). Heap ~12.6 kB only while armed. **SD removal hardening** (section 12): `sdSetPresent()` is the one place presence changes; `sdMount()` does `SD.end()` first; a capture write failure reports `sdcap: write failed after N frames (card removed?) - recording stopped`, closes and hands the ring back via one path (`sd.ioFailed` -> `sdServiceFlush`); a short `sdread` sends an error instead of `sdread_done` (the host used to save the truncated file); `sdinfo` and the `sdOpenCapture`/`sdls` failure paths no longer leave FATFS mounted. **Presence probe** (no card-detect pin): one SPI CMD0 at 400 kHz every 2 s while idle and unmounted, two agreeing readings change state - verified, empty slot R1 0xFF, card 0x01, removal seen within ~4 s; `sdprobe` / `sdface` diagnostics; the `sd card removed|inserted` log drives the host's `sd.mounted`. **LCD faces**: on a presence change a full-screen drawn face on `lv_layer_top()` for 3 s (sad pale blue "SD card out" + reason, happy yellow "SD card in"). Measured: card pulled mid-`sdcap` closed the capture cleanly (619 frames, 166 kB), events buffered 21 rows and flushed them all on re-insert. One of three hot-pulls reset the board with `rst: usb` (likely a supply dip dropping USB, then the reconnect's DTR/RTS - rule 1; not the probe): pull the card gently. Host/dashboards: v2 the default at `/` (`--ui classic`), event-log control + status + `events.csv`/`seen.csv` pulls in both, `sdread` accepts `CARD_TEXT_FILES`, sdcap "recording stopped" clears cap, v2 energy bars regained `cls`, classic `expl`/`age`. Static 76,816 -> 77,272 B. LED blip on an event still undecided (with C10/1.6.1).

## v1.17

Wi-Fi device table 64 -> **96 slots** (+2,368 B static, now 76,816 B; free heap -2.4 kB on every page, capture worst case still ~39 kB). A full table no longer fits the 8 KB TX buffer as one line, so `sendDevices()` emits `{"t":"w"}` in chunks of <= 24 rows, loudest first; the host already merged by MAC, so no host change. Measured: 82 distinct devices in 2 min, ~32 fresh rows per cycle in two lines.

## v1.16.1

small fixes: `BleDev` repacked 56 -> **48 B** (it had 8 B of padding; -384 B static) with `static_assert`s on `BleDev` and `Dev154`; the BLE Devices footer showed a "scan cycle" counter that never moves since discovery runs forever - now `N adv/s`; Spectrum page bars custom-drawn through a shared `BarStrip` (also used by Overview), fixing a clip that hid the top three bars (~2477-2483 MHz); two LCD strings used `·`, which Montserrat lacks (rendered as a box) - LCD text must stay ASCII.

## v1.16

**C1 probe-request mapping**: directed probe requests (a client naming the network it wants) are parsed in `promiscuousCb` (`queueProbe`, body at offset 24, first SSID IE, wildcards skipped, control-stripped) into an 8-slot Wi-Fi-task queue; `serviceProbes()` (host_proto.cpp, loop) suppresses repeats of a (MAC, SSID) pair for 60 s (32-entry table) and emits `{"t":"pr","mac","rssi","ch","ssid"}`. +712 B static. Host `merge_probe` keeps pairs 15 min and groups by SSID because nearly every probing phone randomizes its MAC per burst (measured: 5/5 on first run); both dashboards gained a "Networks being sought" card and a "probing for ..." line on matching client rows.

## v1.15.5

Overview's per-channel strip and the Devices list custom-drawn too (`ovStripDraw`, `devListDraw`; Devices repaints only rows whose FNV fingerprint changed): Overview 87.6 -> 98.9 kB, Devices 91.2 -> 106.4 kB free, pages now span ~99-106 kB; static 74,032 -> 73,904 B. Drawn one-line text needs `LV_TEXT_FLAG_EXPAND` (`lv_draw_label` word-wraps in its box; LONG_CLIP did not). Fixes found on the way: tier-1 (`~`, destination-only) Wi-Fi devices carry rssi 0 and sorted *above* every real transmitter, pushing the loudest real devices off the LCD's 12 rows - `collectWifiRefs()` now sorts them last, and the LCD shows `--` with an empty bar instead of "0" with a full red one.

## v1.15.4

Channels LCD page custom-drawn: its 39 rows x (row + 2 labels + bar), ~156 LVGL objects, became one object painted in `chanGridDraw()` from a 3-byte `ChanCell` snapshot, invalidating only changed cells - free heap on that page 56.1 -> **106.3 kB** (+50 kB), static 74,504 -> 74,032 B; it is now the lightest page and Overview (~87.6 kB) the heaviest. That inverted a risk: a capture ring sized on a light page went under the 24 kB floor on the next step to Overview (measured 16.4 kB), so `showPage()` now records each page's build cost and the ring also leaves `lcdPageHeadroomB()` (step to the heaviest page available in the mode) - USB+SD capture now bottoms out at ~30.8 kB from any starting page. Docs: ROADMAP staleness pass (release, static/heap numbers, serial budgets, WifiDev now one array, C9's EAPOL test was the Authentication subtype, 1.6.2/1.6.4 folded into C4, 1.13 mirror section); `tools/probe_pages.py` names pages from the device's ack.

## v1.15.3

memory hygiene (from the 2026-10 RAM audit, measured on-device): **`cap 1` then `sdcap 1` defeated the 24 kB heap floor** - the ring was floor-checked before the FATFS mount, landing at 16.1 kB free; `sdcap` now calls `refitCapRing()`, which re-sizes an existing ring against post-mount heap without touching either sink (20 -> 11 slots, 30.5 kB), and `ensureCapRing()` now *sizes* the ring to leave the floor standing instead of refusing outright. **Park in spec mode is honoured** (it holds the fine sweep on the parked 15.4 channel's centre bin; full sweeps/`fs` pause while parked) and v2's park control is enabled in spec with a truthful note. BLE device-row serial budget 102 -> 116 (a maxed row is ~110 B; under-budget truncates the line mid-write, rule 6); Wi-Fi stays at 126 because 64 x 126 + 40 is the most the 8 KB TX buffer allows. Dropped the upstream empty-touchpad LVGL indev. LCD page heap swing (~45 kB, Channels the heaviest) documented in §16; the Channels-page redraw is the next RAM lever (ROADMAP).

## v1.15.2

trend gained a second series: band average (mean of channels sampled in the last ~15 s, stored as history tuple element 4 in the `d` handler) drawn as a dashed line under the solid busiest line, with a legend; retitled "Channel load over time". The max pins high in busy air, the average moves. Host+dashboard (kVersion bumped for parity).

## v1.15.1

dashboard: the Wi-Fi "max busy over time" trend read as a solid blob (it plots the busiest channel, which sits high, under a flat 55%-opacity area fill); switched to a fade-out gradient fill (`#trendGrad` in `renderTrend`) so the line/dips read clearly, retitled "Busiest channel over time" with a plain-language `<small>` explainer. Dashboard-only (kVersion bumped for parity).

## v1.15

review pass: spectrum 2.4 GHz energy updates per probe in the dashboard (host `fd` handler writes each per-dwell bin live, not only the full-sweep `fs`; other modes already update per-dwell via the `d` line); BLE dashboard chart replaced - per-device RSSI bars (redundant with the table) became a "What's advertising" category breakdown (Find My/AirTag, AirPods/audio, phone/Mac/Watch, beacon, other); LCD 802.15.4 Overview/Channels footer reads "nodes <n>" (live `collect154Refs` count) instead of a Wi-Fi-only "APs 0".

## v1.14

mirror UI: the LCD-mirror panel moved into the dashboard's left control rail (172x320 canvas fits its width); added `page next|prev` (`stepPage()` in lcd_ui.cpp, BOOT-tap emulation) with **Page ‹ ›** buttons so the interface can be stepped from the browser - the practical answer to fast-updating screens (spectrum) tearing over the serial link.

## v1.13.1

mirror fix: a strip dropped during the initial scan (collides with a data-refresh flush, overruns the TX buffer) left a permanent black/stale band because mid-scan drops were ignored; `mirrorNoteDrop()` now flags a re-scan that repeats until a pass completes with no drop (self-heals).

## v1.13

live LCD mirror: `mirror 1|0` streams the 172x320 screen to the host over USB serial (default off, not persisted). The LVGL flush callback (`mirrorOnFlush` in host_proto.cpp, hooked from `Lvgl_Display_LCD`) emits each repainted region as an `M x y w h <base64 RGB565-LE>` line; `serviceMirror()` (lcd_ui.cpp, called from the loop) paces a full re-send one ~10-row strip per loop so a bulk repaint never overruns the 8 KB TX buffer; no extra device RAM (piggybacks the existing partial draw buffer). Host `handle_mirror` reassembles a framebuffer, serves `/screen.bin` + `/api/screen`; the v2 dashboard paints it on a canvas (Start-mirror card). Section 19.

## v1.12.2

boot-photo UX: removed the page-wrap picture flash (`showLogoSplash` on wrap in `pollButton()`) - tapping through pages no longer flashes a random picture when it loops from the last page back to the first, and so no longer briefly captures taps into picture-stepping; pictures now appear only at boot and on the hold-walk photo stop after Spectrum.

## v1.12.1

public-release housekeeping + a boot-splash fix: MIT `LICENSE` + `NOTICE` (MIT covers only this project's work; the upstream PierreGode/WaveshareESP32C6LCD base ships no license of its own, so its derived portions are carved out, not relicensed) and a README Credits section for the C6->C5 lineage; `build.sh` auto-regenerates `bandwatch/boot_logos.h` from `bootlogo/boot_*.png` when they change (missing header, newer PNG, or count != `K_BOOT_LOGO_COUNT`; `LOGOS=always`/`never` override). Fix: `showBandSplash()` now hides the boot-photo overlay (and clears `logoThenModeCard`), so tapping BOOT during a committed-mode splash - e.g. after releasing the hold-walk on Spectrum, before it hands off to the scan page - no longer drops into the photo loop; it used to draw the mode card over a still-"active" logo and `logoActive()` only tested the hidden flag.

## v1.12

boot photos: eight pictures from `bootlogo/` embedded as RGB565 in flash (`bandwatch/boot_logos.h`, generated by `tools/img2c.py`; ~860 KB of flash, no RAM cost - LVGL draws the const array directly). A random one shows full-screen for 2 s at boot before the mode card; the BOOT hold-walk gains a photo stop after Spectrum (the last mode) - release there and it lingers 5 s while taps step through the pictures in order; tapping past the last page flashes one on the way around. Timings: `kBootLogoMs` / `kWrapLogoMs`.

## v1.11

roadmap batch one (C3+C6+C8), measured +16 B static total (80,544 -> 80,560), flashed and verified on-device: **C3** NVS settings persistence (`settings.cpp` - band mode + addr1/blescan/specstep/snap survive a power-cycle; park and hunt/deauth deliberately not persisted so a reboot stops transmitting and never boots silently parked; transient RAM only, the `Preferences` handle closes after each load/save); **C6** top talker per dwell (`Accum.bestMac`/`bestRssi`, one instance read by `sendDwell` before `resetAccum`, so `"top"`/`"trssi"` on the `d` line cost no per-channel RAM; serial budget 380->420; both dashboards show a Top-talker column joined to the device table); **C8** least-busy readout (`quietestChannel()`, LCD Overview footer + a Quietest-channel tile on both dashboards; UI-only). Then a RAM-reclaim pass (same release) cut static from 80,560 -> **74,480 B** (-6,080, back under the old ~80 kB line, ~6 kB more free heap - boot "heap after wifi" ~83 -> ~88 kB): **(1)** LVGL draw buffer `buf1` divisor `W*H/14` -> `/21` (7,862 -> 5,241 B; PARTIAL render just paints more chunks, invisible for this UI - raise back toward /14 if the mode splash flickers); **(2)** the device-table listing path no longer copies whole tables into a 4 kB `DevSnap` union - `collect*Refs()` copies a compact `DevRef{rssi,idx,key[8]}` per fresh device under the lock and sorts those (one `g_devRefs[64]` = 640 B), then `fetch*Dev()` re-validates each slot and copies out one full record; a slot evicted/reused between collect and fetch fails the identity check and is skipped that cycle (saves ~3.3 kB). The static-RAM gate for future work: free heap at peak binds, not the static number - the tightest peak is SD capture (was ~31.9 kB free vs the 24 kB `kMinFreeHeapB` refuse-floor; now ~6 kB higher). Remaining reclaim levers: device-table slot counts (`kWifiDevSlots` 64->48 ~2 kB, costs tracking capacity), or the ESP-IDF port for the ~59 kB Wi-Fi/BLE stack that is otherwise fixed.

## v1.10

LCD mode splash + layout pass: a band change (button hold-walk, host `band` command, or boot) flashes a full-screen name card for the new mode before its scan page takes over; holding BOOT walks the six cards at a 700 ms cadence and release commits the one on screen (hunt page stops the hunt first, then walks). Every widget width re-checked against the Montserrat glyph tables (header right labels to font 12 so `park 165 USB` fits, overview title `Bandwatch`->`Activity`, Top-3 and channels-grid columns widened/clipped, dual-band header reads `BOTH chNN`). Host: `sdread` name guard tightened to 39 chars - `sdread /<name>` is 8 + len chars and the device line buffer holds 47, so a 40-char name lost its last char.

## v1.9

dashboard v2 + host sdread reassembly + firmware review pass: a fresh left-rail dashboard (`host/dashboard2.html`, no tabs - controls grouped in a sticky rail, one scrolling main column) runs *in parallel* with the classic one, both always served (`--ui v2` or `host/run-v2.sh` puts v2 at `/`, the other at `/classic` or `/v2`, cross-linked). The host now reassembles the `S <n> <base64>` file chunks the device has streamed since v1.3 but nothing ever collected (they fell through to the JSON parser and flooded the log pane): `handle_sd_chunk` buffers them, `sdread_done` writes the file into the captures dir, `GET /file?name=` serves it for download, `POST sdread <path>` validates against the card naming scheme. Firmware review: `sendInternalKick()` seq encoding now matches `sendKickFrame()` (seq<<4 low/high bytes, clear fragment nibble - the old code put the seq low-nibble in the fragment field and repeated every 256 frames); the dead spec branch in `finishDwell()` dropped (v1.8's early return into `specFine[]` made it unreachable); `noteHuntHit()` on the Wi-Fi/BLE paths checks `hunt.kind == 0`, symmetric with 15.4's `kind == 1`, so a 15.4 key hunt no longer matches a stale MAC; `cap`/`sdcap` refuse in spec mode (no RX armed) instead of recording into a file that never grows.

## v1.8

fine spectrum: spec mode now sweeps 2400-2483 MHz in a configurable 1/2/5 MHz step (default 2) by tuning the 15.4 synth off the channel grid via `ieee802154_ll_set_freq()` (below the public channel API, but an inline HAL function - cleaner than the deauth blob patch; off-grid reads verified against known APs). ~42 bins at 2 MHz vs the old 16 channels, full-band incl. edges; still 2.4 GHz only (no 5 GHz energy). Freq-based serial messages `fs` (full sweep) / `fd` (per-dwell cursor), `specstep` command, dashboard resolution toggles, LCD maps the bins onto 42 fixed bars. Section 18.

## v1.7.5

dashboard refactor (review nit): factor the four bar charts' shared grid/axis + tooltip scaffolding into gridAxis()/chartTips(); no behavior change. Dashboard-only (kVersion bumped for parity).

## v1.7.4

dashboard: warn when every 2.4 GHz bin's noise floor is elevated (> -85 dBm; healthy ~-110) - a strong signal desensitising the 15.4 receiver, or a stuck radio state - instead of showing a misleading flat spectrum. Dashboard-only (kVersion bumped for release parity).

## v1.7.3

energy now tracks the Wi-Fi band: the 128 us ED window listened only ~5% of each dwell and missed bursty Wi-Fi (a strong but idle AP showed no energy); widened kEdDurationSym to 2 ms (~50% coverage) so energy peaks line up with the actual APs.

## v1.7.2

review pass (carries all 1.7.1 energy fixes): RF-energy bar palette no longer starts at near-black (vanished on dark grey); Overview chart/tooltip no longer mislabel spec's 15.4 bins as 5 GHz; hello/system page report dwellMs() not the fixed kDwellMs; host guards the energy24/activity5/spec_hist caches with dlock (were mutated on the reader thread while HTTP threads read them); hopActive() helper.

## v1.7.1

fix: the 1.7 "faster scan" hopped channels every 60 ms from the loop, but the 15.4 radio needs ~>100 ms to settle after set_channel, so energy detect returned a stuck ~-40 dBm on every channel (flat, not tracking real occupancy). Reverted to the 120 ms UI-timer hop (~2 s sweep); floor back to ~-110 dBm with real structure. Also: combined 2.4-energy + 5-GHz-activity dashboard view, auto-ranged dBm axis with peak-hold, Wi-Fi/BLE activity bars, responsive-layout fix.

## v1.7

spectrum analyzer: a `spec` mode runs the 15.4 radio's energy-detect primitive across channels 11-26 for a true 2.4 GHz RF-power reading (dBm, no decode) - the only real noise-floor measurement this board exposes; 5 GHz has no equivalent. The host correlates per-bin energy against recently-decoded Wi-Fi/BLE/Zigbee to flag *unexplained* energy (an emitter with an unknown/undecodable protocol - evidence, not an ID; the radio cannot demodulate it). New LCD spectrum page + dashboard spectrum tab (bars, waterfall, unexplained-energy list) - section 18.

## v1.6

the deauth attack transmits: an external witness (`tools/witness/`, a second ESP32 in monitor mode) proved neither path reached the air, for two unrelated reasons - the raw path was rejected by libnet80211's subtype gate, and the internal slot path radiates nothing for *any* subtype, so the section 9 offsets are wrong rather than merely fragile. Raw TX is now the default and `tools/deauth/patch_raw_tx.py` patches out the gate post-build; 289 deauth frames witnessed on air at -38 dBm - section 11.

## v1.5.5

merge of the two 1.5.x lines: 1.5.3/1.5.4 were built on the pre-split monolith, so `dca` and the association tracking were ported onto the module layout (1.5.3/1.5.4 firmware tags predate the split and do not build from this tree).

## v1.5.4

the 1.5.3 `dca` UI was unreachable end-to-end (four independent breaks); station-to-BSS association added to make it work, at zero static-RAM cost - section 17

## v1.5.3

targeted deauth (`dca`)

## v1.5.2

deauth sequence numbers fixed (all frames in burst now have unique seq; previously identical 0x00), diagnostic logging added for TX path verification - attack still does not work, root cause unidentified (see DEVELOPER.md §11)

## v1.5.1

review pass: firmware split into modules behind `bandwatch_core.h`, capture refused below a 24 kB free-heap floor, `sdls` reports total/sent (host shows the card's file list) - sections 3 and 16

## v1.5

surveillance OUI flagging, addr1 tier-1 sightings, non-blocking sdread - section 15

## v1.4.2

capture telemetry in BLE mode, sdcap ack key, LCD record light

## v1.4.1

BLE MAC byte-order fix + auto scan policy

## v1.4

BLE advertising capture as pcap link type 256, BLEScan replaced with direct NimBLE discovery - section 13

## v1.3

microSD pcap recording (device writes the pcap; independent USB/SD sinks; sdls/sdread; on-demand mount) - section 12

## v1.2.5

deauth frame was a QoS-Null, not a deauth (fixed); attack still does not work - see docs/DEVELOPER.md section 11

## v1.2.4

review pass: capture-ring leak on mode change, RF-string sanitising, host robustness

## v1.2.3

deauth crash fix + dead-man's-switch timeout + serial command-injection fix

## v1.2.2

deauth attack (spoof a BSSID, kick its stations)

## v1.2

802.15.4 (Zigbee/Thread), pause/freeze tables

## v1.1

BLE, device tables, hunt

## v1.0

sweeps + LCD + dashboard + pcap
