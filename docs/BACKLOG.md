# Bandwatch backlog

The one list of open work. `ROADMAP.md` keeps the design write-ups and history; this file says **what is left, where
it lives, and when it is done**. Pick an item, do it, delete its row (or move it to "Done" with the version), and
note the release in `CLAUDE.md`'s version history as usual.

Every item has a **Done when** line, and wherever possible a test to add, so work lands with a regression check
(`tests/README.md`: tier 1 host, tier 2 firmware build, tier 3 on the board).

Priority: **P1** wrong or misleading behaviour today · **P2** gap a user will notice · **P3** improvement / decision.
Type: bug · verify (built, not proven on hardware) · decision (needs a call before code) · feature.

State as of **v1.19.1** (2026-10-07). Finished items move to **Done** at the end, with the evidence.

---

## Bugs

### B1 · Dashboard v2 hides a running deauth while a hunt is active — P1, bug
**Symptom.** With a hunt and a deauth running at the same time, the v2 action bar shows only the hunt. The deauth's
frame counter, the "rejected by the driver" count and the unpatched-image warning are hidden, and the bar's single
Stop button stops the hunt only. A running transmit is then invisible in the default UI. The retired classic
dashboard showed both (`#huntCard` + `#deauthCard`, `renderHunt()` / `renderDeauth()` in `host/dashboard.html`): use
it as the reference - `git show v1.18.2:host/dashboard.html`.

**Where (host/dashboard2.html, v1.18.2 line numbers):**
- Markup: `<section class="card actbar" id="actBar">` (~line 259). One status block (`#actTitle`, `#actNum`,
  `#actUnit`, `#actMeter`, `#actSub`), one middle block (`#actMeta`, `#actTrend`, `#actWarn`) and one `#actStop`
  button.
- Render: `renderAct(s)` (~line 776). `actMode = h ? 'hunt' : k ? 'deauth' : ''` picks one mode and fills the shared
  elements, with an `if (h) {...} else {...}` per mode.
- Stop: the `#actStop` click handler (~line 525) branches on `actMode`.
- Styles: `.actbar` (~line 67, grid of 3 columns; ~line 170 for the narrow layout). `[hidden]` already forces
  `display: none` (~line 148).

**Steps:**
1. **Markup:** turn the one bar into two independent bars with the same inner layout, e.g. `#huntBar` and
   `#deauthBar`. Give each its own title/number/unit/meter/sub/meta/stop elements, with ids prefixed per bar
   (`#huntNum`, `#deauthNum`, ...). Only the hunt bar needs the trend `<svg>`; only the deauth bar needs the warning
   `<div>`. Reuse the `.actbar` class for both so the styles apply unchanged.
2. **Render:** split `renderAct(s)` into `renderHuntBar(s)` and `renderDeauthBar(s)`. Each shows or hides its own bar
   from its own field (`s.hunt`, `s.deauth`). Move the existing two branches into them as they are, retargeted to the
   new ids. Drop the global `actMode`.
3. **Stop buttons:** give each bar its own handler. The hunt one posts `{cmd:'hunt', mac:null}`. The deauth one keeps
   the existing `armPending('deauth', <button>, 'Stopping…')` + `{cmd:'deauth', mac:null}`.
4. **Call sites:** replace the `renderAct(s)` call in `render()` with the two calls. Grep for `renderAct` and
   `actMode` to catch every reference.
5. **Order:** put the deauth bar first (above the hunt). A transmit in progress should be the most prominent thing on
   the page.

**Done when:**
- With `s.hunt` and `s.deauth` both set, both bars are visible with their own numbers, and each Stop stops only its
  own activity.
- With only one set, the page looks as it does today.
- The narrow (mobile) layout still stacks cleanly.

**Test:** add a tier-1 render test in `tests/host/`. It can load `dashboard2.html`'s script under a small stub DOM, or
assert on the HTML string a render produces. Feed a snapshot with both `hunt` and `deauth` set, and assert both bars
exist and neither is hidden. Add a second case with only `hunt` set.

### B2 · Deauth table buttons can stay stuck on "…" after an attack stops — P2, bug
**Symptom.** Click Deauth on a table row, then stop the attack, and every row's Deauth button can keep showing a
disabled "…" until the page reloads.

**Why:** `deauthBtn()` (~line 1132) renders "…" while `pending.deauth` is set. The only code that clears it is inside
`renderAct()`'s deauth branch (~line 806), which runs only *while a deauth is active*. So when the attack ends
before the latch is released (or a hunt is active, B1), nothing ever clears it.

**Steps:**
1. Move the latch release out of the deauth-only branch into code that runs on every render. The current condition is
   right: release after 6 s, or when the reported on/off state (`!!s.deauth`) differs from `prev.deauthOn`.
   `prev.deauthOn = !!s.deauth` must also update on every render, active or not.
2. Re-enable whichever Stop button `armPending` disabled. After B1 that is the deauth bar's Stop.
3. Do B1 first. B2's fix lands naturally in the new `renderDeauthBar()`, as long as the release runs before its
   "hidden when no deauth" early return.

**Done when:** start a deauth from a row, stop it from the bar or the row, and the row buttons return to "Deauth"
within one poll (1 s).

**Test:** a tier-1 render test that sets `pending.deauth`, renders a snapshot with `deauth: null`, and asserts the row
button is no longer "…".

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

### V2 · The SD faces showed on the LCD during a real pull — P3, verify (needs your eyes)
- Both faces were checked through the mirror (`sdface 0|1`), and the removal/insertion log lines were checked
  during a real pull. Nobody has yet confirmed seeing the faces on the panel itself during a pull.
- **Done when:** someone watches the LCD through a pull and a re-insert.

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

### I1 · Run the regression suite automatically — P2, feature
- `tests/run_offline.sh` exists. Wire it into a git pre-push hook (`.git/hooks/pre-push` or a `tools/hooks/`
  script plus a setup line in `setup.sh`), so tiers 1-2 run before every push.
- **Done when:** a push with a failing host test or a static-RAM jump past `tests/firmware/static_ram_ceiling.json`
  is refused.

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
| C10 | Permit-join LED blip | Blocked on D1 |
| C11 | Host CSV export | Host-only |
| C12 | ESP-IDF port | The exit ramp: BLE extended advertising, the fixed ~59 kB stack |
| 1.6.1 | Alerting | Blocked on D1 |
| 1.6.3 | Control without a host | Needs a button mapping for sdcap/events; the hold gesture is taken by the mode walk |
| 1.9 | SD pull stops at download | v2 can pull and download, but cannot delete or rename on the card |
| 1.10 | Mode splash is fire-and-forget | The splash does not reflect a radio that failed to start |

## Known limits (documented, no action planned)
- Whether a deauth actually disconnects a real station is untested (DEVELOPER §11).
- BLE 5 extended advertising is invisible with the prebuilt core (rule 7; C12 would lift it).
- Surveillance OUI matches are evidence, not proof, and have never been seen live.

---

## Done (most recent first)

| Item | Version | Evidence |
| --- | --- | --- |
| **I3** tier-3 baseline recorded | v1.19.1 | `tests/device/RESULTS.md` (19/19 at v1.18.2, 26/26 at v1.19) |
| **V3** event-log heap at peak load | v1.19 | `events 1` + `cap 1` + `sdcap 1`, `both` band, walking every LCD page: minimum **25,172 B** free (Overview), floor 24,576 B. Holds by design - the capture ring shrinks to keep the floor - but the margin is thin (~0.6 kB); DEVELOPER §16/§20 |
| **V1** no duplicate `new` rows after a card swap | v1.18 fix | `/events.csv` on the card: 174 `new` rows, 166 distinct. All 8 duplicates are from one boot at 12:54-12:58, the first pull test, which ran *before* the re-insert fix (each MAC logged before the pull and again after). 100+ later rows, including a pull on the fixed firmware, have no duplicates. Caveat: that later pull lasted only ~17 s |
| **D4** `/seen.csv` rotation + card files downloadable/deletable | v1.19 / v1.19.1 | `T11SdRm` on the board (deleted a real pcap; refused the file being recorded); grouped "SD card files" card in v1.19.1. Rotation timing still open (V5) |
| **D2** classic dashboard retired | v1.19 | `/classic` 301 -> `/`; last copy at tag v1.18.2 |
| **D1** LED alert blips | v1.19 | `T12Alerts` on the board (`alerts` round-trip, `ledtest` acks); the colours themselves still need eyes (V6) |
| **B3** `page prev` stuck on unavailable pages | v1.18.2 | `T08Page.test_page_prev_is_inverse_of_next` |
| **B4 (likely)** "silent" serial port | v1.19.1 | two processes on one port split the bytes; host now opens it exclusively. Kept open above until confirmed |
