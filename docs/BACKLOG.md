# Bandwatch backlog

The one list of open work. `ROADMAP.md` keeps the design write-ups and history; this file says **what is left, where
it lives, and when it is done**. Pick an item, do it, delete its row (or move it to "Done" with the version), and
note the release in `CHANGELOG.md` as usual.

Every item has a **Done when** line, and wherever possible a test to add, so work lands with a regression check
(`tests/README.md`: tier 1 host, tier 2 firmware build, tier 3 on the board).

Priority: **P1** wrong or misleading behaviour today · **P2** gap a user will notice · **P3** improvement / decision.
Type: bug · verify (built, not proven on hardware) · decision (needs a call before code) · feature.

State as of **v1.19.3** (2026-10-07). Finished items move to **Done** at the end, with the evidence.

---

## Bugs

### B4 · USB serial went silent while the firmware kept running — P2, bug (likely a second reader; confirm)
- **Seen once, 2026-10-07, v1.19:** right after the full tier-3 suite finished (26/26, `T99NoReboot` passed), the next
  session on the port got **zero bytes**: no idle traffic and no reply to `info`/`sdls`/`sdread`. The LCD kept updating
  (the firmware was alive). `esptool --before no-reset` could not reach the ROM loader (not download mode). Only the
  RESET button recovered it; afterwards everything worked (hello `up 60`, sdread fine).
- **Likely explained (same day):** a second process reading the port. macOS lets two programs open it; they split the
  bytes, each sees "readiness to read but returned no data" or nothing at all, and esptool gets "No serial data
  received" - exactly what was seen. The user had a host running alongside; with one reader there were 0 errors in 30 s.
  v1.19.1 opens the port **exclusively** in the host (a second one is refused with a clear message). Keep this item
  open until the device suite + a fresh open no longer reproduces it.
- **Earlier suspects:** (1) the device suite's port handling - `tests/device/board.py` clears HUPCL so closing the port does
  not drop DTR; the next open by another process may leave the USB-Serial-JTAG CDC in a state where the device thinks
  no host is reading (Arduino HWCDC) and silently discards TX, or RX stops; (2) an HWCDC TX stall after a long burst
  at a full buffer (`setTxTimeoutMs(0)`).
- **Reproduce:** run the device suite, then open the port from a fresh process with DTR/RTS asserted and send `info`.
  Repeat a few times; also try a fresh open after `power_profile.py`.
- **Done when:** the cause is known and either fixed (firmware: e.g. detect "no host" via `HWCDC` connected state and
  reset the TX side; or the suite restores HUPCL) or documented with a recovery that needs no RESET press.

## Verify on hardware (built, not yet proven)

### V5 · `/seen.csv` rotation time on hardware — P3, verify
- Put a `/seen.csv` with ~5000 distinct globally-unique MAC lines on the card, `events 1` (or re-insert while armed):
  expect `seen.csv rotated: 5000 -> 2048`, `ev.file` 2048. Time the attach (the loop task is busy for it - LCD and
  hopping pause). Pull both files and check the content.
- **Done when:** the time is in DEVELOPER §20; if it is more than ~1 s, consider rotating in chunks across loops.

### V6 · LED alerts on real hits — P3, verify
- `ledtest` covers the patterns. Still unseen: a real surveillance OUI in range (or a test OUI in `/surveil.csv`) and
  a Zigbee coordinator opening joins (`band 154`). Also: with events armed in a busy place, `new` blinks stay <= 1 per 2 s.
- **Open choice from D4:** deleting `/seen.csv` while armed also drops MACs still waiting to be appended, so those
  devices count as new once more. Fine, or keep them?

### V4 · Hot-pulling the card reset the board once (`rst: usb`) — P3, verify
- One of three pulls (the second; the first during `sdcap` and the third under the reconnecting watcher did not)
  coincided with a USB-peripheral reset. Note that a second program reading the port (B4) can look similar but shows
  *no* uptime reset; this one did reset (`up` restarted, `rst: usb`). Not a panic, and not the probe (DEVELOPER §12). A
  likely cause is a supply dip briefly dropping the USB link.
- **How:** about 10 pulls with `card_watch`-style monitoring (it reconnects, and logs `REBOOT` when uptime goes
  backwards). Once with a powered hub and once direct.
- **Done when:** the rate is known. If it is high, note it in the README ("pull the card gently / power off
  first"). A hardware fix (bulk capacitance near the slot) is out of scope for the firmware.

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
| C5 | Patrol mode (auto round-robin) | Keeps the spectrum's "explained" sources fresh; covers the 1.7 time-separation gap |
| C7 | Hunt by SSID | Small; reuses the hunt ack shape |
| C9 | Deauth refinements (rate, auto-stop) | Read the corrected EAPOL detection in ROADMAP C9 first |
| C11 | Host CSV export | Host-only |
| C12 | ESP-IDF port | The exit ramp: BLE extended advertising, the fixed ~59 kB stack |
| 1.6.3 | Control without a host | Needs a button mapping for sdcap/events; the hold gesture is taken by the mode walk |
| 1.9 | SD card file management | The dashboard can pull, download and delete (`sdrm`, D4); renaming on the card is not possible |
| 1.10 | Mode splash is fire-and-forget | The splash does not reflect a radio that failed to start |

## Known limits (documented, no action planned)
- Whether a deauth actually disconnects a real station is untested (DEVELOPER §11).
- BLE 5 extended advertising is invisible with the prebuilt core (rule 7; C12 would lift it).
- Surveillance OUI matches are evidence, not proof, and have never been seen live.

---

## Done (most recent first)

| Item | Version | Evidence |
| --- | --- | --- |
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
