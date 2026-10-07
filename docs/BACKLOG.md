# Bandwatch backlog

The one list of open work. `ROADMAP.md` keeps the design write-ups and history; this file says **what is left, where
it lives, and when it is done**. Pick an item, do it, delete its row (or move it to "Done" with the version), and
note the release in `CLAUDE.md`'s version history as usual.

Every item has a **Done when** line, and wherever possible a test to add, so work lands with a regression check
(`tests/README.md`: tier 1 host, tier 2 firmware build, tier 3 on the board).

Priority: **P1** wrong or misleading behaviour today · **P2** gap a user will notice · **P3** improvement / decision.
Type: bug · verify (built, not proven on hardware) · decision (needs a call before code) · feature.

State as of **v1.18.2** (2026-10-07).

---

## Bugs

### B1 · Dashboard v2 hides a running deauth while a hunt is active — P1, bug
- **Where:** `host/dashboard2.html`, `renderAct()` - the single action bar picks the hunt first.
- **Symptom:** with both active, the deauth status is not shown: its frame counter, the "rejected by the driver"
  count and the unpatched-image warning `#actWarn`. A running transmit is then invisible in the default UI.
  Classic shows both (`#huntCard` + `#deauthCard`), so classic is the reference.
- **Fix outline:** render the two states independently. Either two bars, or one bar with two sections. The deauth
  part must stay visible whenever `s.deauth` is set.
- **Done when:** with `s.hunt` and `s.deauth` both set, both statuses are visible, and either can be stopped from
  the bar.
- **Test:** add a tier-1 render check (the jsdom approach in the v2-parity work, or a DOM-string check) that feeds a
  snapshot with both set and asserts both status elements are present and not hidden.
- *Note: an automated agent was stopped by a safety classifier while editing this area, so it was left for a
  person.*

### B2 · Deauth table buttons can stay stuck on "…" after an attack stops — P2, bug
- **Where:** `host/dashboard2.html`, `renderAct()` - the code that clears `pending.deauth` only runs while a deauth
  is active.
- **Symptom:** click Deauth, stop it, and the row buttons keep the pending "…" label until reload.
- **Fix outline:** clear the pending state whenever the reported state matches what was requested, including "no
  attack".
- **Done when:** starting then stopping from the dashboard returns the buttons to normal within one poll.
- **Test:** tier-1 render check, as for B1.

### B3 · `page prev` skipped unavailable pages forward — fixed in v1.18.2
Kept here for one release so the history is visible. `showPage(n, dir)` now skips in the step's direction. The
tier-3 test `T08Page.test_page_prev_is_inverse_of_next` covers it. Delete this row in the next release.

---

## Verify on hardware (built, not yet proven)

### V1 · Event log: devices first seen while the card was out are not logged as "new" twice — P2, verify
- **Where:** `bandwatch/events.cpp`, `attachCard()` re-inserts `ev.newMacs` into the baseline after a reload. This
  was added after the removal test, where `base` read 32 instead of about 52.
- **How:**
  1. `events 1` with a card in.
  2. Pull the card for about 60 s in a busy area.
  3. Re-insert it and wait for a flush.
  4. `sdread /events.csv`: no MAC should have two `new` rows.
- **Done when:** checked on the board, and the ROADMAP C4 "Not verified" note is removed.
- **Test:** this could become a tier-3 test only with a way to simulate removal (see I4). Until then it is manual.

### V2 · The SD faces showed on the LCD during a real pull — P3, verify
- Both faces were checked through the mirror (`sdface 0|1`), and the removal/insertion log lines were checked
  during a real pull. Nobody has yet confirmed seeing the faces on the panel itself during a pull.
- **Done when:** someone watches the LCD through a pull and a re-insert.

### V3 · Event-log heap at peak load — P2, verify
- The armed log holds about 12.6 kB of heap (baseline set, row buffer, pending MACs). Nobody has measured it
  together with the worst case.
- **How:** run `events 1` + `cap 1` + `sdcap 1` on the Overview page (the heaviest), then step through every page.
  `tests/device` `T04Capture.test_cap_and_sdcap_keep_heap_floor` does the stepping. Run it with events armed and
  record the minimum.
- **Done when:** the minimum free heap is recorded in DEVELOPER §16/§20 and stays at or above 24 kB. If not,
  shrink `kBaseCap` or make `ensureCapRing()` account for the armed log.

### V4 · Hot-pulling the card reset the board once (`rst: usb`) — P3, verify
- One of three pulls coincided with a USB-peripheral reset. Not a panic, and not the probe (DEVELOPER §12). A
  likely cause is a supply dip briefly dropping the USB link.
- **How:** about 10 pulls with `card_watch`-style monitoring (it reconnects, and logs `REBOOT` when uptime goes
  backwards). Once with a powered hub and once direct.
- **Done when:** the rate is known. If it is high, note it in the README ("pull the card gently / power off
  first"). A hardware fix (bulk capacitance near the slot) is out of scope for the firmware.

---

## Decisions needed (no code until decided)

### D1 · LED feedback for events — P3, decision
C4 novelty/surveillance hits, C10 permit-join, and 1.6.1 alerting all want the single WS2812. ROADMAP 1.6.1 proposes
a short distinct double-flash that yields back. **Decide:** the priority order in `driveLed()` and the blip pattern.
Then implement all three together.

### D2 · Retire the classic dashboard? — P3, decision
v2 is the default and now covers the classic gaps except B1. Classic has no LCD mirror, no SD pulls and no
"last seen" caches. **Decide:** keep classic at `/classic` indefinitely, or remove it after B1 and drop the
duplicate maintenance (every protocol change currently touches two dashboards).

### D3 · Mirror on pages that out-run the link — P3, decision
- The BLE Devices page repaints about 220 KB/s of base64, so the mirror never gets a complete frame there; it shows
  "repairing" (DEVELOPER §19). Options:
  - (a) accept it, as today;
  - (b) while mirroring, re-sort the LCD device list less often (e.g. 2 s instead of 500 ms);
  - (c) a mirror frame-rate cap.
- (b) changes the device's own UI behaviour while mirroring; decide whether that is acceptable.

### D4 · `/seen.csv` growth policy — P3, decision
The file is never trimmed; only its newest 2048 entries are loaded. It is about 18 B per MAC, so years of walks are
still small, but a card used long-term grows forever. **Decide:** trim on load when the file exceeds N entries, or
leave it.

---

## Improvements

### I1 · Run the regression suite automatically — P2, feature
- `tests/run_offline.sh` exists. Wire it into a git pre-push hook (`.git/hooks/pre-push` or a `tools/hooks/`
  script plus a setup line in `setup.sh`), so tiers 1-2 run before every push.
- **Done when:** a push with a failing host test or a static-RAM jump past `tests/firmware/static_ram_ceiling.json`
  is refused.

### I2 · Raise test coverage where bugs were found — P2, feature
- **Tier 1:**
  - a dashboard render harness committed to `tests/host/` (jsdom or a stub DOM), so B1/B2 and future v2 changes
    have checks;
  - `events.csv` row-format parsing.
- **Tier 3:**
  - the C1 `pr` line shape (needs a directed probe nearby, so skip if none is heard);
  - `w` chunking with more than 24 devices;
  - a no-truncated-lines soak in BLE mode with the mirror on.

### I3 · Record the tier-3 baseline — P3, feature
After each release, save the device-suite summary (pass counts, minimum heap seen, soak line counts) to
`tests/device/RESULTS.md`, so the next run can be compared against it.

### I4 · Simulated card removal for tests — P3, feature
A diagnostic command (e.g. `sdsim 0|1`) that makes `sdMount()` fail and the presence probe report "absent"
without touching the card. It would let tier 3 cover V1 and the event log's no-card path unattended.

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
