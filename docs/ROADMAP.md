# Bandwatch roadmap and open items

Planned work, open questions and known gaps. Current release: **v1.5.4**.

Entries say what is actually known, including what has *not* been verified. Anything measured is quoted
with its numbers; anything assumed is labelled as such.

---

## 1.6 — untethered surveillance detection

**Goal:** make the detection added in 1.5 useful with no laptop attached. Detection already runs on the
device (`kSurvOuis` is matched in firmware precisely so the LCD can flag without a host); what is missing is
everything around it.

### 1.6.1 Alerting — *highest value, smallest change*

A marker on a page you have to be looking at is not an alert. Needs an active signal when a surveillance OUI
is matched.

The board has **no buzzer**, so the RGB LED is the only channel, and it is already four-way contended in
`driveLed()`: deauth blink > hunt distance > record pulse > busy score. A fifth state needs a slot in that
hierarchy, and the awkward case is real — recording *and* detecting at once is exactly when you are walking
around with the device in your hand.

Proposed: a detection wins the LED briefly (distinct fast double-flash, a few seconds) then yields back to
whatever was showing, rather than becoming a new permanent state. Not yet agreed.

### 1.6.2 Hit logging to SD

Untethered means nobody is watching the dashboard, so hits must persist: MAC, category, tier, RSSI, channel,
timestamp, appended to a file on the card. This is the actual deliverable of a walk-around.

Blocked on the memory-budget question below: a 32-entry in-RAM ring is ~512 B, which does not fit under the
current static-RAM rule. Alternatives are writing straight through to SD on each hit (simplest, but mounts
FATFS on every hit) or buffering only a handful of entries.

### 1.6.3 Control without a host

`sdcap`, `time` and band/park are all host commands today. Recording-start was deliberately made
host-only in 1.3 (the alternative was explicitly considered and rejected then), but untethered use changes
that trade-off. Needs a BOOT-button path, and a decision about the LCD page/long-press mapping, which is
already carrying "tap = page, hold = mode, hold on Hunt = stop hunt".

Also unresolved untethered: the device has no RTC, so with no host to send `time <epoch>` the hit log and
pcap filenames fall back to a counter and uptime-based timestamps.

### 1.6.4 OUI list on the SD card

A 64-prefix table goes stale, upstream already withdrew two Flock prefixes as Ubiquiti false positives, and
reflashing in the field is not an option. Reading `/surveil.csv` at mount and falling back to the built-in
table would make the list updatable by dropping a file on the card. Folds naturally into 1.6.2.

---

## Memory budget — replace the proxy with a measurement

`CLAUDE.md` rule 4 says static RAM must stay "well under ~80 KB". Origin: commit `265d955` (1.2), after real
OOM crashes — `abort() … lock_init_generic` (newlib could not allocate a mutex) and a store fault in
`lv_obj_class_create_obj` (LVGL `malloc` returned NULL). Someone cut static RAM, the crashes stopped, and the
line stuck. **It is an empirical heuristic, not a derived limit, and nobody has re-derived it since 1.2.**

The mechanism behind it is real: static allocations and the heap share one 320 KB DRAM pool, so every static
byte costs a heap byte 1:1. Note the build output is actively misleading here — it reports *"leaving 248104
bytes for local variables"*, but measured free heap at runtime is ~83 kB, because the Wi-Fi/BLE driver stacks
and FreeRTOS task stacks take the rest.

What actually binds is **free heap at peak concurrent load**. Measured on hardware:

| state | free heap |
| --- | --- |
| idle, Wi-Fi | ~83 kB |
| idle, BLE | ~95 kB |
| SD capture (tightest normal state) | **31.9 kB** |
| BLE + SD capture | 32.3 kB |
| SD capture with the pre-1.3 ring ordering | 12.5 kB — where LVGL page rebuilds start failing |

Proposal: replace the static-RAM proxy with a **minimum-free-heap floor** (~25 kB in the worst concurrent
case: capture ring + FATFS + an LVGL page rebuild landing together). It is checkable at runtime, tied to the
actual failure mode, and would let the firmware *refuse* to start a capture whose projected heap falls below
the floor rather than crashing. Under such a rule the 512-byte hit log stops being a rule violation.

Current static usage is 79,584 B — about 416 B under the existing line. That headroom is the edge of an
unverified budget, not a wall. 1.5.4 is a worked example of the squeeze it causes: adding a station's BSSID
to `WifiDev` should have been 6 bytes, but that struct is in two `kWifiDevSlots` arrays and would have
rounded 64 → 68, i.e. 512 B, which does not fit. Repacking the struct to exactly 64 bytes bought 3 bytes of
former padding for free instead — a good outcome here, but the reason only 3 of the 6 bytes are on the device
is this unverified line, not anything about the radio.

---

## Open questions (need a decision)

- **"If popular"** — from the 1.6 request, meaning unclear. Could be: filter to only the categories you care
  about; only flag devices seen repeatedly rather than once (which would cut false positives substantially);
  or gauge whether the feature gets used before investing further.
- **"Google lenses"** — implemented in 1.5 as **Meta/Ray-Ban smart glasses** (5 OUIs), on the assumption that
  camera glasses were meant. Google Glass is discontinued and no OUI set was found for it. Assumption not
  confirmed.

---

## Known gaps and smaller items

- ~~**`sdls` can silently truncate.**~~ Fixed in 1.5.4: the reply carries `listed` and `skipped`, and the
  host says "N entries omitted" instead of showing a short listing as if it were complete.
- **Surveillance detection is unproven against real hardware.** The matcher is unit-tested against known
  values from both upstream sources plus a negative control, but no Ring/Flock/Axon device has ever been in
  range during testing. The first live hit is the real test.
- **BLE 5 extended advertising is invisible.** `CONFIG_BT_NIMBLE_EXT_ADV` is not set in the fixed sdkconfig,
  so devices advertising only on extended or coded PHY do not appear at all. Lifting this means moving the
  project to ESP-IDF (rule 7).
- **Device names are environment-capped, not code-capped.** Measured: 31 distinct advertisers, 3 ever
  broadcast a name. Most nearby traffic is Apple continuity and non-connectable beacons that never answer a
  `SCAN_REQ`. No scanning strategy changes that.

---

## Blocked

- **Deauth does not work and the root cause is unidentified.** PMF, DFS and promiscuous mode are all ruled
  out by measurement, and the frame is now a byte-correct `0xC0 0x00` deauthentication. Whether anything
  radiates at all is *still unverified in both directions* — the only witness available during testing was a
  macOS Wi-Fi scan, and macOS redacts SSIDs, so the `txtest` beacon self-test could not be read.
  **Needs a second radio** (another ESP32 in promiscuous mode, or a USB adapter in monitor mode). Everything
  else here is guesswork until that exists. Full investigation log in
  [`DEVELOPER.md`](DEVELOPER.md) §11.
- **SoftAP injection PoC is inconclusive**, not negative — its own state handling is incomplete. Do not cite
  it as evidence either way.

---

## Process note

Five releases (1.2.5 → 1.4.2) shipped with no README version entry, because doc edits used a bare
`str.replace()` anchored on the previous release's bullet: once one anchor was missing, every later edit
silently matched nothing and the failure cascaded. Fixed in `088a128`. **Assert that an anchor exists before
replacing it** — a no-op edit that reports success is worse than a crash.

It happened again at 1.5.3: no README entry, no `CLAUDE.md` history line, and this file still said "current
release v1.5". Restored in 1.5.4. Two further 1.5.3 lessons worth keeping:

- **The tag was one commit early.** `v1.5.3` pointed at the feature commit, not the `kVersion` bump that
  followed it, so a build from that tag reported `1.5`. Before tagging, check that the tag's own tree agrees:
  `grep -q "\"$(git describe --tags --abbrev=0 | tr -d v)\"" bandwatch/bandwatch.cpp`.
- **A feature was shipped, tagged and documented without ever being run.** The `dca` dashboard path was
  broken in four independent places at once — a filter on a field that did not exist, buttons with no way to
  show them, a MAC format the device rejects, and an unhandled acknowledgement. Each one alone makes the
  feature do nothing. Reaching a new control *through the UI it ships with*, once, would have caught all four.
