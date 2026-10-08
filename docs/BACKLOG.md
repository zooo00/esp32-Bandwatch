# Bandwatch backlog

The one list of open work. `ROADMAP.md` keeps the design write-ups and history; this file says **what is left, where
it lives, and when it is done**. Pick an item, do it, delete its row (or move it to "Done" with the version), and
note the release in `CHANGELOG.md` as usual.

Every item has a **Done when** line, and wherever possible a test to add, so work lands with a regression check
(`tests/README.md`: tier 1 host, tier 2 firmware build, tier 3 on the board).

Priority: **P1** wrong or misleading behaviour today · **P2** gap a user will notice · **P3** improvement / decision.
Type: bug · verify (built, not proven on hardware) · decision (needs a call before code) · feature.

State as of **v1.20.0** (2026-10-08). Finished items move to **Done** at the end, with the evidence.

---

## Bugs

None open (B4 closed in v1.20, see Done).

## Verify on hardware (built, not yet proven)

### V7 · Patrol mode on the board — P2, verify
- Flash v1.20, run `tests/device` T13Patrol (custom legs `both:5,ble:5`: hand-off seen in hello + `{"t":"pt"}` in BLE,
  refusals both ways, `band` ends it). Then `patrol 1` for a few full cycles with the dashboard open: the Patrol card
  counts down, the channel views collapse/return like a manual mode click, the LCD header reads `patrol SPEC 12s`.
- Free heap from the `s`/`ble`/`fs` lines before and after each hand-off stays within a few hundred bytes (no leak per
  cycle); leave it walking ~30 min with `events 1` and check `T99`-style no reboot.
- T13Patrol passed on the board (v1.20.0, 2026-10-08, 32/32 run).
- **Done when:** the heap numbers per hand-off are in DEVELOPER §22 and a ~30 min `patrol 1` run shows no reboot.

### V6 · LED alerts on real hits — P3, verify
- `ledtest` covers the patterns. Still unseen: a real surveillance OUI in range (or a test OUI in `/surveil.csv`) and
  a Zigbee coordinator opening joins (`band 154`). Also: with events armed in a busy place, `new` blinks stay <= 1 per 2 s.

---

## Decisions needed (no code until decided)

### D3 · Mirror on pages that out-run the link — P3, decision **deferred** (2026-10-07: "wait")
- The BLE Devices page repaints about 220 KB/s of base64, so the mirror never gets a complete frame there; it shows
  "repairing" (DEVELOPER §19). Options:
  - (a) accept it, as today;
  - (b) while mirroring, re-sort the LCD device list less often (e.g. 2 s instead of 500 ms);
  - (c) a mirror frame-rate cap.
- (b) changes the device's own UI behaviour while mirroring; decide whether that is acceptable.

---

## Improvements

### I1 · Run the regression suite automatically — P3, feature (host tier done)
- Done: `tools/pre-push` (installed per `AGENTS.md`) refuses force-pushes/deletions and runs the host tier
  (`tests/host`) before every push.
- Left: the firmware tier (compile + static-RAM ceiling in `tests/firmware/static_ram_ceiling.json`) is not in the hook
  because it needs the toolchain and is slow; run `tests/run_offline.sh` before a release.
- **Done when:** a static-RAM jump past the ceiling is refused at push time (or decide it stays a release step).

### I2 · Raise test coverage where bugs were found — P2, feature (partly done)
- Since written: `tests/host/test_sdrm.py` (10), alerts command/protocol tests, `T11SdRm` and `T12Alerts` on the board.
  Still to do:
- **Tier 1:**
  - a dashboard render harness committed to `tests/host/` (jsdom or a stub DOM), so B1/B2 and future v2 changes
    have checks;
  - `events.csv` row-format parsing.
- **Tier 3:**
  - the C1 `pr` line shape (needs a directed probe nearby, so skip if none is heard);
  - `w` chunking with more than 24 devices;
  - a no-truncated-lines soak in BLE mode with the mirror on.

### I4 · Simulated card removal for tests — P3, feature
A diagnostic command (e.g. `sdsim 0|1`) that makes `sdMount()` fail and the presence probe report "absent"
without touching the card. It would let tier 3 cover V1 and the event log's no-card path unattended.

### I6 · Measure power draw per mode — P2, verify
- Needs an inline USB-C power meter (or an INA219 on the 5 V line); the board has no current sensor and macOS only
  reports the 500 mA USB allocation.
- **How:** `BANDWATCH_PORT=... python3 tools/power_profile.py` holds each mode (5g / 2.4g / both / BLE / 802.15.4 /
  spectrum / + SD capture / + event log) for 45 s, asks for the meter reading, and prints battery life for 2/5/10 Ah
  power banks.
- **Done when:** the table is in README ("Battery life") and DEVELOPER §1.
- **Follow-up lever if it is high:** the LCD backlight (GPIO10) is always on; a dim / timeout setting is likely the
  biggest saving outside the radio.

### I5 · 802.15.4 devices in the event log — P3, feature
C4 covers Wi-Fi and BLE only. 802.15.4 extended addresses are stable and globally unique, so novelty would work.
- **Where:** `ieee154.cpp` slot creation -> `eventFlag()` with a 15.4 flag. Hash `key[8]` instead of the 6-byte
  MAC, and give the row `radio` = `154`.
- **Done when:** a new Zigbee/Thread node produces one `new` row.

---

## Roadmap features not started (details in ROADMAP.md)

| ID | Feature | Notes |
| --- | --- | --- |
| C2 | Ghost AP (beacon, then probe responder) | Transmits: needs the dead-man's switch pattern; stage 1 before stage 2 |
| C9 | Deauth refinements (rate, auto-stop) | Read the corrected EAPOL detection in ROADMAP C9 first |
| C12 | ESP-IDF port | The exit ramp: BLE extended advertising, the fixed ~59 kB stack |
| 1.6.3 | Control without a host | Needs a button mapping for sdcap/events; the hold gesture is taken by the mode walk |
| 1.9 | SD card file management | The dashboard can pull, download and delete (`sdrm`, D4); renaming on the card is not possible |
| 1.10 | Mode splash is fire-and-forget | The splash does not reflect a radio that failed to start |

## Known limits (documented, no action planned)
- Deauth works on real stations (owner-confirmed 2026-10-08); PMF networks ignore it by design (DEVELOPER §11).
- BLE 5 extended advertising is invisible with the prebuilt core (rule 7; C12 would lift it).
- Surveillance OUI matches are evidence, not proof, and have never been seen live.

---

## Done (most recent first)

| Item | Version | Evidence |
| --- | --- | --- |
| **V5** `/seen.csv` rotation time | v1.20 | `seengen 5000` 522 ms; rotation 5000 -> 2048 **582 ms**, whole attach **1,312 ms** (measured v1.20.0, 2026-10-08, `T14SeenGen`); 2048 kept, header and 83 B rows (the test checks them). ~1.3 s loop pause once per rotation: no chunking needed |
| **B4** USB serial went silent (TX only) | v1.20 | Reproduced on v1.20.0 (2026-10-08) mid device suite, right after the capture tests: zero bytes out, no other program on the port (exclusive open), but the board still ran commands (`page next` and `ledtest new` visibly worked) - so RX alive, TX stalled. Cause: HWCDC moves its ring into the IN FIFO only from IN_EMPTY; one landing while the FIFO is not writable is cleared and never returns, and HWCDC only re-flushes on a link drop. Fix: `serviceSerialTx()` flushes the FIFO + re-arms IN_EMPTY after 1 s without draining (+24 B static), `txk` in hello, `txkick` by hand. Next full suite: 32/32 with `txk` = 2 (two stalls recovered). (A first close earlier the same day blamed a second reader - wrong.) |
| **V4** card hot-pull resetting the board | v1.20 | Closed on the owner's observation (2026-10-08): pulls "seem to work fine"; the one `rst: usb` stays unexplained (likely a supply dip). README still advises pulling gently |
| **D4 choice** pending MACs on a `seen.csv` delete | v1.20 | Decided: dropped. A deliberate delete means "start novelty over" |
| **C11** host CSV export | v1.20 | `dashboard2.html`: a *CSV* button on the Wi-Fi, BLE, Zigbee and probe tables exports the rows shown (DEVELOPER §5). Checked with a scratch Node harness over the pure builder (hostile `=HYPERLINK` SSID, comma/quote/newline name, null-RSSI `dest_only` row, non-ASCII names, RFC 4180 round-trip) and the host tier; not yet clicked in a browser against a live board |
| **C5** patrol mode (auto round-robin) | v1.20 | `patrol 1` / `patrol <mode>:<sec>,...` / `patrol 0`, `pt` in hello + `{"t":"pt"}`, LCD header, dashboard Patrol card (DEVELOPER §22). Offline only: compiled (+56 B static), host tests `tests/host/test_patrol.py`; T13Patrol written but not yet run on the board (V7) |
| **C7** hunt by SSID | v1.20 | `huntssid <name>` / `huntssid 0`, DEVELOPER §23. Offline only: firmware builds (77,192 B static, +8), `tests/host/test_hunt_ssid.py` (route validation, ack/hello parsing, AP list), dashboard render harness. The two-APs walk test (park follows the stronger) still needs hardware |
| **B1** dashboard hid a running deauth while a hunt was active | v1.19.3 | `dashboard2.html` renders the hunt and deauth bars independently, so both show at once, each with its own Stop. Code review only; not yet watched on hardware with both running |
| **B2** deauth row buttons stuck on "…" | v1.19.3 | the `pending.deauth` latch is released on every render whenever no deauth is running, not only inside the active-deauth branch. Code review only (the render harness in I2 is still to do) |
| **C10 + 1.6.1** permit-join LED blip / alerting | v1.19 | shipped as D1 (§21) |
| **I3** tier-3 baseline recorded | v1.19.1 | `tests/device/RESULTS.md` (19/19 at v1.18.2, 26/26 at v1.19) |
| **V3** event-log heap at peak load | v1.19 | `events 1` + `cap 1` + `sdcap 1`, `both` band, walking every LCD page: minimum **25,172 B** free (Overview), floor 24,576 B. Holds by design - the capture ring shrinks to keep the floor - but the margin is thin (~0.6 kB); DEVELOPER §16/§20 |
| **V2** SD faces on the LCD during a real pull | v1.18 | Watched on the panel 2026-10-07 through a real pull and re-insert: user reported the LCD behaviour "good" (no defects noted). Log showed `sd card removed` at 14:58:37; the monitor's 3-minute window did not capture the `inserted` line, so that half rests on the observation alone |
| **V1** no duplicate `new` rows after a card swap | v1.18 fix | `/events.csv` read off the card 2026-10-07 (v1.19.1): 183 `new` rows, 175 distinct. The 8 duplicates are the same pre-fix ones from the first pull test (12:54-12:58); this session's 8 rows (after a power-cycle and a pull + re-insert: `pending` 4 held with `written` 0, then `base` 171 -> 176, `written` 8) are all distinct. Caveat: the board power-cycled before the pull, so the monitor missed it live and the pull's duration is unknown - a long pull within one session is still unproven |
| **D4** `/seen.csv` rotation + card files downloadable/deletable | v1.19 / v1.19.1 | `T11SdRm` on the board (deleted a real pcap; refused the file being recorded); grouped "SD card files" card in v1.19.1. Rotation timing still open (V5) |
| **D2** classic dashboard retired | v1.19 | `/classic` 301 -> `/`; last copy at tag v1.18.2 |
| **D1** LED alert blips | v1.19 | `T12Alerts` on the board (`alerts` round-trip, `ledtest` acks); the colours themselves still need eyes (V6) |
| **B3** `page prev` stuck on unavailable pages | v1.18.2 | `T08Page.test_page_prev_is_inverse_of_next` |
| **B4 (likely)** "silent" serial port | v1.19.1 | two processes on one port split the bytes; host now opens it exclusively. Kept open above until confirmed |
