# Bandwatch roadmap and open items

Planned work, open questions and known gaps. Release history is in [`CHANGELOG.md`](../CHANGELOG.md); as of v1.19.3 (v1.12 boot photos, v1.13-1.14 LCD mirror,
v1.15.x review/dashboard passes and the 2026-10 RAM audit, v1.16 C1, v1.17 the 96-slot Wi-Fi table, v1.18 C4 + SD
removal hardening, v1.18.x mirror frames + regression suite, v1.19 LED alerts / classic retired / card-file
management, v1.19.1 grouped card-file view + exclusive serial port, v1.19.3 review fixes). Shipped: C3 + C6 + C8 (v1.11), C1 (v1.16), C4 (v1.18, which also delivers 1.6.2 and 1.6.4);
C10 + 1.6.1 LED alert blips (v1.19, D1); C5 patrol mode (v1.20); C2/C7/C9/C11 still candidates, C12 the exit ramp.

**Open work is tracked in [BACKLOG.md](BACKLOG.md)**; this file keeps the design write-ups behind it.

Entries say what is actually known, including what has *not* been verified. Anything measured is quoted
with its numbers; anything assumed is labelled as such.

---

## Candidate features (C1-C12)

Twelve candidates collected for analysis. They are independent unless noted, and each is sized to ship
as its own release or in small batches. Written so an analyst who has not seen the code can evaluate:
costs name where they come from, and anything that needs a decision sits under *Open questions* instead
of being assumed.

**How to analyze.** Rank by value-for-effort against this project's identity (portable Wi‑Fi/BLE/15.4
surveillance + interference tool; one time-shared radio). Check the RAM math in the preamble — static
is under 3 kB below the line since v1.18 (77,184 B at v1.19.3), and free heap at peak is what binds. Anything adding a protocol line costs a three-way sync (firmware sender ↔ host
`merge_*` in `bandwatch_host.py` ↔ the §4 table in DEVELOPER.md); UI-only candidates skip that cost.
The quick hits (C6–C11) are cheap enough to batch; the big ones each justify their own release.

**Preamble — constraints any candidate must respect:**

- **One radio, six modes.** `5g / 2.4g / both / ble / 154 / spec` are exclusive; `setBandMode()`
  (`bandwatch.cpp`) is the only swap point and calls `releaseCapture()` on every change (pcap link type
  differs per radio). New mode = extend `BandMode`, `kBandName[]`, `chanEnabled()`, `setBandMode()`,
  LCD pages, `sendHello`'s `chs`, dashboard buttons (checklist in DEVELOPER §6).
- **RAM is the constraint.** No PSRAM; statics and heap share ~320 kB DRAM. Static usage: v1.10 was
  **80,544 B** (+952 B since 1.5.5, ~95% of it spectrum mode, not the splash - DEVELOPER §16; over the round "well under ~80 kB" line, CLAUDE.md rule 4); the v1.11 reclaim pass (smaller
  LVGL buffer + the `DevRef` listing path replacing the 4 kB `DevSnap` copy) brought it to 74,480 B,
  v1.15.4 to 74,032 B; v1.17's 96-slot Wi-Fi table took it to 76,816 B and v1.18 (C4 event log + SD presence
  probe + LCD card faces) to 77,272 B, and v1.19's LED alerts to **77,392 B** (measured from the build; `tests/firmware` gates it at 77,800 B).
  Peak load measured at v1.19 - event log armed + USB + SD capture, every LCD page - left **25,172 B** free (floor
  24,576 B). The C4 log also holds ~12.6 kB of *heap* while
  armed (DEVELOPER §16/§20), included in the peak figure above. Free heap at runtime (`both`, v1.15.5) depends on the
  LCD page: Overview ≈98.9 kB (the heaviest), System ≈102.3, Devices ≈106.4, Channels ≈106.4; BLE ≈88 kB (v1.15.2).
  USB + SD capture bottoms out at ~27.8 kB on any page (v1.15.5): `ensureCapRing()` sizes the ring to leave `kMinFreeHeapB` = 24 kB *plus*
  `lcdPageHeadroomB()` (the step to the heaviest page), and `sdcap` re-fits a ring `cap 1` made before the
  FATFS mount (`refitCapRing()`). `WifiDev` is packed to exactly 64 B behind a `static_assert` (`devices.h`) in
  one 96-slot array (64 until v1.17), so +1 byte of *padding* costs 384 B static. For reference: `BleDev` = 48 B × 48 slots (repacked from 56 in v1.16.1,
  `static_assert` since then, like `Dev154`), `Dev154` = 24 B × 48, `CapFrame` = 1,610 B × 4-20.
- **Three contexts + one ISR.** Loop task (LVGL timer + `Bandwatch_Loop`, ~2 ms cadence), Wi‑Fi task
  (`promiscuousCb` in `wifi_sniff.cpp`), NimBLE host task (`bleGapEvent` in `ble_scan.cpp`), and the 802.15.4
  true ISR. Radio paths are `IRAM_ATTR`, spinlocked, no heap / no Serial. SD writes and capture draining run
  on the loop task only — a radio-side event must flag-then-drain (existing pattern: `s_edReady` in
  `ieee154.cpp`, `bleScan.activeUntilMs`).
- **Serial never blocks.** 8 kB TX buffer, `setTxTimeoutMs(0)`, every line checks `serialRoom()` and is
  dropped whole *if its budget is honest* - a line that passes the check and then outgrows its budget is
  truncated mid-JSON. Since v1.19.3 every line, acks and errors included, budgets from the real JSON-escaped
  string lengths (or a derived true worst case) instead of estimates; read `host_proto.cpp` for the current
  numbers rather than copying them here. Wi‑Fi device rows go in chunks of ≤24 rows since v1.17 (a 96-row table
  would not fit 8 kB as one line). New per-line fields grow these.
- **No RTC.** Timestamps come from the host's `time <epoch>`; without it, uptime-based times and counter-named
  files. Anything persisting to SD inherits this.
- **Anything that transmits needs a dead-man's switch** (`kDeauthMaxMs` = 5 min auto-stop) — there is no other
  way for the board to stop itself. TX power values are in 0.25 dBm units (82 = 20.5 dBm; DEVELOPER §9,
  corrected v1.19.3).
- **LED priority in `driveLed()`** (`lcd_ui.cpp`): deauth blink > *alert blip* > hunt distance > record pulse >
  busy score. Since v1.19 (D1) alerts are transient blips (`led_alert.cpp`, DEVELOPER §21) that win the LED for
  300-500 ms and yield back, rate-limited to one per 2 s; they never override a deauth. A new *permanent* state still
  needs a slot; a new alert kind should ride the blip layer (`LedAlertKind`, one row in `kPatterns`).
- **SD shares the LCD's SPI bus** (CS GPIO4, 20 MHz), loop-task only, budgeted at `kSdBudgetUs` = 8 ms per
  loop; mounting FATFS costs ~30 kB and is done on demand (`sdMount()`/`sdUnmount()`, §12).

| # | Name | One-liner | Static RAM cost | New protocol lines? | Est. effort |
|---|------|-----------|-----------------|---------------------|-------------|
| C1 | Probe-request mapping ("seeking") | sleepy devices reveal which network they want | ~0.2–0.4 kB (dedup table) | yes: `pr` events | S–M |
| C2 | Ghost AP mode (+ probe responder) | attract sleepers, catch their handshakes | <100 B + heap scratch while active | yes: `ghost` cmd/ack, counters in `d` | M |
| C3 | NVS persistence | boot where you left off; settings survive reboot | transient ~0.5–1 kB, none resident | no (maybe a log line) | S |
| C4 ✅ v1.18 | Event log → SD CSV (+ novelty baseline) | persistent hits + "new device here" without a host | 16-entry queue 160 B + ~12.6 kB heap while armed (1.18 total incl. presence/faces: +456 B static) | yes in the end: `ev` status line + `events` cmd | M |
| C5 ✅ v1.20 | Patrol mode (auto round-robin) | keeps the spectrum's "known emitters" fresh; passive logging | +56 B (leg table, timer, 64 B command line) | yes: `patrol` cmd/ack, `pt` in hello + a `{"t":"pt"}` status line | S–M |
| C6 | Top talker per dwell | name the loudest voice on each channel | +≈8 B in `Accum` (one instance) | field added to `d` line | S |
| C7 | Hunt by SSID | "where's my network" without knowing its MAC | 34 B while active, heap-allocated | reuses `hunt` ack shape; new command | S–M |
| C8 | Least-busy readout | quietest channel on Overview / dashboard | 0 — computable from today's `s` rows | no | S (UI only) |
| C9 | Deauth refinements | rate control + auto-stop when the handshake is caught | a few bytes | fields in `d`, new command | S–M |
| C10 ✅ v1.19 | Permit-join LED blip | a Zigbee door opening = brief double-flash | 0 (or +48 B, see entry) | no | S |
| C11 | Host CSV export | one-click device-table download | n/a (host side) | no | S (UI only) |
| C12 | ESP-IDF port | unlocks BLE 5 extended adv + re-tunable driver config | re-derive §9 offsets and the §16 budget | wire format unchanged | L (a migration, not a feature) |

### C1 — Probe-request mapping ("seeking" SSIDs)  ✅ SHIPPED v1.16


**Status: shipped v1.16.** Decisions taken on the open questions: (1) 32 pairs × 60 s suppress (+712 B static with the
8-slot Wi-Fi-task queue); (2) host-expire after 15 min, no stop lines; (3) one line per SSID - in practice a probe
request carries a single SSID IE, so only the first is parsed; (4) host-only for v1, no LCD. First on-air run: 5
directed probes in 90 s, every one from a **randomized MAC** (new per burst), so the host groups by SSID ("Networks
being sought" card, the dashboard) and also hangs `seeking` on any device row with a matching MAC.

**What.** A probe request is a client announcing which network(s) it wants: source MAC plus up to ~3
SSID IEs. Bandwatch already *counts* them in `promiscuousCb()` but never parses them (only beacons get
IEs, via `parseBeaconIes()`). Parse them and the device table can say *Flock camera — probing for
'HomeNet'*: the sleepy devices we only see as tier‑1 `addr1` destinations suddenly name their network.

**Why it fits.** Pure RX in all three Wi‑Fi modes, including while parked on a hunt channel; it completes
the evidence chain 1.5 built (tier‑1 sightings) without any transmit duty.

**How (sketch).** In `promiscuousCb()`, add an `fc0 == 0x40` branch alongside the existing
`isBeacon = (fc0 == 0x80 || fc0 == 0x50)` check: walk SSID IEs from payload offset **24** to `sigLen - 4`
(beacons start at 36 because of timestamp/interval/capability fields; probe requests have none). Emit one
event per (MAC, SSID) pair: `{"t":"pr","mac":"aa:..","ssid":"HomeNet"}`. Device-side dedup keeps it cheap
and nearly stateless: a small fixed table of recently-reported pairs — 32 × {mac[6], ssidHash u16, lastMs}
≈400 B static (or 16 ≈200 B) — suppresses re-emits for ~60 s. The SSID text rides in the line itself, so
only a hash is stored on device; the host keeps real history per device (`merge_pr()` → `seeking` field).
LCD: optional "probing for X" on the Hunt page for the target while parked.

**Cost & constraints.** ~0.2–0.4 kB static for the dedup table (the one resident cost; if that is too much,
16 entries or heap-allocate only in Wi‑Fi modes). Serial: `pr` lines are small (~50 B) and rate-limited by
design — N probing clients emit at most ~N·3 per minute. No impact on the packed 64-byte `WifiDev` (that is
the point of event-streaming instead of adding a field to it).

**Open questions.** (1) Dedup table size + suppress window: 32 × 60 s is a guess; measure real probe chatter first.
(2) Emit *stops* (`pr …,0`) or let the host expire? Host-expire is cheaper. (3) Multiple SSIDs per request — one
line each (recommended, simpler merge) or an array in one line? (4) LCD presence in v1, or host-only first?

**Verify.** Park on a channel with a known phone; `pr` lines should name its home SSID — Wireshark alongside as
ground truth. Host renders *probing for* under the right MAC; stop the probe source and watch entries expire.

### C2 — Ghost AP mode (beacon, then probe responder)

**What.** Deauth pushes clients off their network; a ghost pulls sleepers in. A `ghost <ssid>` flag on the
Wi‑Fi modes parks and holds up our own BSSID, beaconing at ~10 Hz so any scanner sees it — stage 2 answers
probe requests with probe responses echoing the *requested* SSID(s), so sleeping clients associate to us and
emit EAPOL handshakes we can capture and crack offline. The README's "run a capture alongside to catch WPA2
handshakes" becomes one workflow.

**Why it fits.** Every piece exists: `sendTestBeacon()` (`deauth_diag.cpp`) proved this exact radio transmits
beacons from an unassociated, promiscuous STA (323 witnessed at −45 dBm), and the attack's park + TX-power-raise
+ dead-man's-switch is the template to copy. It covers the attract half of the surveillance playbook that deauth
only does in the push direction.

**How (sketch).** Stage 1: `ghost <ssid>` / `ghost 0`; while active, reuse the park machinery (`setPark`, same
last-writer-wins dance as hunt/deauth), raise TX power to the maximum (84 = 21 dBm) like `startDeauth()`, and re-send a beacon from the
loop task at ~10 Hz — an extended `sendTestBeacon` with a configurable SSID (≤32 B) and a fixed ghost MAC (the
txtest SA `{0x02,0xBA,0xAD,0xBE,0xEF,0x01}` is a fine default). Count our echoes vs probe requests mentioning us;
report `gh` counters in the dwell line like `da`. Stage 2: in `promiscuousCb()`, when `fc0 == 0x40` and ghost is
active, flag the request (client MAC + SSIDs) for the loop task to answer with a probe response (`fc0 = 0x50` —
already beacon-class today). Security IEs advertised: start **Open**; stretch goal is echoing RSN from the last-seen
beacon of an AP with that name (we store `sec`/`pmf` per AP) so WPA2 clients actually run a PSK exchange.

**Cost & constraints.** Beacon buffer ~64 B static (txtest's lives on the IRAM stack; ghost runs from the loop task,
so scratch can be heap or plain stack). The responder needs one pending-request slot (client MAC + up to 3 SSIDs ≈50 B)
drained in `Bandwatch_Loop()`. LED: take the deauth-blink slot while active — it is a transmit duty too. Dead-man's
switch reuses the `kDeauthMaxMs` shape with its own start timestamp. Total well under 1 kB if stage‑2 scratch is
heap-only-while-active.

**Open questions.** (1) Park or hop? Park = focused attack on a suspected channel; hop = wider net but handshakes
fragment across channels. Recommend park by default, optional channel argument like hunt. (2) Beacon interval: 100 TU
(the txtest value ≈ 10 Hz) vs 200 TU — measure probe traffic before optimising airtime. (3) Open security in v1 lets
clients associate but not handshake — acceptable for stage 1? (4) Ghost and capture running simultaneously should be
the norm: any ring-sizing interaction to check? (5) Answer association requests or let them time out? Not answering
means WPA2 clients retry auth a few times and give up — enough EAPOL M1s, arguably cleaner.

**Verify.** The witness (`tools/witness/`) sees our beacons; a phone with Wi‑Fi on but no network in range starts
probing (stage-1 counters move). Stage 2: capture running + one WPA2 sleeper → EAPOL frames from its MAC in the pcap.
`txstat`-style sent/responded/failed counters so it never rests on a counter alone (§11's lesson).

### C3 — NVS persistence (boot where you left off)  ✅ SHIPPED v1.11

**Status (shipped, verified on-device).** Implemented in `settings.cpp` (`loadSettings`/`saveSettings`), wired into
`Bandwatch_Init()` and the `band`/`addr1`/`blescan`/`snap`/`specstep` handlers + the button-walk commit. Persists
band mode + the four scalar policies; park and hunt/deauth deliberately not persisted (open question 1 resolved:
mode only, park hops). Save is on user-commit, not per walk-step. **Static-RAM cost measured: +8 B** (the
`Preferences` handle; NVS code is flash, not RAM). Boot log line `settings restored: band X`. **Verified:**
`band 2.4g` → `reboot` → `info` reported `band 2.4g` (default would be 5g).

**What.** Power-cycling today resets everything: band back to `5g`, park cleared, `blescan auto`, `specstep 2`,
`snaplen 1600`, `addr1 on`. Persist settings so a walk-around device boots ready.

**Why it fits.** The whole 1.6 roadmap is untethered use; re-talking the board through serial after every
power-cycle is exactly what that should not need. Also the cheapest QoL here: no new protocol, no transmit duty,
no resident RAM (the NVS handle opens only for a moment).

**How (sketch).** `Preferences` (esp_nvs wrapper), one namespace; load in `Bandwatch_Init()` before the first
`sendHello()`, save on change from the loop task. Keys: `bandMode`, `parkedIdx` (−1 = hopping), `trackAddr1`,
`bleScan.mode`, `specStepMhz`, `capSnapLen`. A version key guards schema changes; empty/corrupt NVS falls back to
today's defaults. Transient heap while the handle is open (~0.5–1 kB — measure on first impl); closed right after,
so nothing stays resident. Hunt/deauth deliberately *not* persisted: a reboot should stop transmitting (same safety
argument as the dead-man's switch).

**Open questions.** (1) Persist park or clear it? A parked boot is convenient until you forget why — maybe persist
mode only and let park default to hopping. (2) Time/`epochValid`: no, the host re-sends on connect. (3) The v1.10
splash card could carry a small "restored" tag so it is visible that state came back.

**Verify.** Set band/park/settings → `reboot` → compare hello fields to pre-reboot. Corrupt NVS (erase the partition
on a sacrificial board) → defaults return and it still boots.

### C4 — Event log to SD CSV (+ novelty baseline)  ✅ SHIPPED v1.18

**Status: shipped v1.18** (`events.cpp`, DEVELOPER §20; card removal/insertion hardening that came with it in §12).
Decisions taken on the open questions: (1) **events wait during `sdcap`** (and during `sdread`): rows keep
buffering in a 2 KB heap buffer and flush after the capture, overflow counted in `drop` - no second open file, no
second 4 kB buffer; (2) rotation is the boring one: at 1 MB `/events.csv` becomes `/events.old.csv` (one previous
file kept); (3) the baseline in RAM is the **newest 2048 entries** of `/seen.csv` (sorted 32-bit FNV hashes,
`kBaseCap` 2560 leaves room for 512 new this session); the file was never trimmed in 1.18 - since the D4 follow-up
an attach that finds > 4096 entries rotates it (old file -> `/seen.old.csv`, newest 2048 rewritten, DEVELOPER §20);
(4) LED: decided in v1.19
(D1) with C10 and 1.6.1 - a `new` row gives a white single blink (DEVELOPER §21). Deviations from the sketch below: the queue is 16 entries, filled when a
device *slot is created* (not per frame); the baseline set, row buffer and pending appends are heap while armed
(~12.6 kB), not static; the card is mounted only per flush (every 60 s, at 16 rows, or at 48 pending new MACs),
not held; randomized MACs (Wi-Fi locally-administered, BLE random) never count as new but still log as `surv`;
with no card novelty is suspended (`wait`), surveillance rows still buffer, and a mount is retried every 30 s or
as soon as the presence probe sees a card; a failed mount/write marks the card lost and the next mount reloads
that card's baseline (re-inserting this session's pending new MACs). It did add a protocol line after all - the
`{"t":"ev"}` status every 5 s while armed, plus `ev` on `hello` and the `events` ack. `/surveil.csv` (1.6.4) is read
once at boot by `sdProbeAtBoot()`, not at every mount.
**Measured:** first flush at 16 rows, epoch-stamped CSV correct, `seen.csv` 31 entries after ~80 s; card pulled
mid-`sdcap` and re-inserted lost no rows (21 buffered, `written: 52` after). Presence probe verified: empty slot
R1 = 0xFF, card in 0x01, removal logged within ~4 s worst case (measured 1.1 s after the first absent reading).
One of three hot-pulls reset the board (`rst: usb` - a USB-peripheral reset from a likely supply dip, not a panic,
not the probe); pull the card gently. **Verified (v1.19.1, from the card's `/events.csv`):** the re-insert of pending new MACs - the only duplicate
`new` rows (8) come from the first pull test, before the fix; none after, across a later pull. The LED blip shipped in v1.19 (DEVELOPER §21). **D4 follow-up (card-file management):** every
card file (pcaps, `events.csv`/`.old`, `seen.csv`/`.old`, `surveil.csv`) is downloadable *and deletable* from dashboard
v2 (`sdrm <name>`, refused for the file being recorded, during an `sdread` or mid-flush; deleting `seen.csv` while
armed restarts novelty; DEVELOPER §12), and `/seen.csv` rotates as above. `sdrm` passed on the board in the v1.19
tier-3 run (`T11SdRm`, 26/26, `tests/device/RESULTS.md`); the rotation's timing on hardware is still open
(BACKLOG V5). Original entry below.

**What.** One append-only `/events.csv` on the card: `epoch_ms, kind, id, rssi, ch, extra`. v1 kinds: surveillance
hits (category + tier) and **novelty** — a MAC not present in a `/seen.csv` baseline dropped on the same card. The
second one is what "walk around with it in your hand" actually needs: an unknown device appearing is the alert no
dashboard can give you untethered, and it is the concrete shape 1.6.1/1.6.2 are circling.

**Why it fits.** `sd_sink.cpp` already has every discipline worked out — buffered writes under `kSdBudgetUs`,
on-demand mount, epoch fallback for timestamps/names. Events reuse that writer; only the trigger side is new.
Card-resident config also lands 1.6.4 (`/surveil.csv`) in the same mount path: read both files at `sdMount()`,
fall back to the built-in tables when absent.

**How (sketch).** Radio-side matches already happen under lock — `trackWifiDevice()` does the OUI match; novelty is a
set lookup on the same MAC — so each just *flags* a pending event into a small fixed queue (8 × ~24 B ≈ 200 B static),
drained by `Bandwatch_Loop()`, which writes through a second buffered writer (same 4 kB buffer pattern as sd_sink, or
share the existing one while events are suppressed during capture — see open question). Baseline: read `/seen.csv` at
mount into a transient heap set of MAC hashes; keep it resident only while events are enabled; append new hits on
unmount so each session grows the baseline.

**Cost & constraints.** Pending queue ≈200 B static + writer buffer 4 kB heap-while-active (or shared). Two open files
at once (capture + events) is *the* RAM question — FATFS per-file overhead is small but a second 4 kB buffer is real;
suppressing events during `sdcap` is the cheap escape. CSV rows are tiny (~40 B), so even busy hit rates write far less
than frames do.

**Open questions.** (1) Events concurrent with capture, or mutually exclusive? (2) Rotation: truncate at ~1 MB with a
header rewrite vs `events-2.csv` — pick the boring one. (3) Baseline growth policy for a card in the field long-term
(oldest-first eviction bounds the file). (4) LED blip on novelty: which `driveLed()` slot, or ride 1.6.1's transient
double-flash proposal (never agreed)? Decide once together with C10.

**Verify.** Seed `/seen.csv` with known MACs, wave a stranger past it, read the CSV off-card (`sdread` already works).
Check writer byte-discipline against `capinfos` like §12 did for captures.

### C5 — Patrol mode (auto round-robin)  ✅ SHIPPED v1.20

**Status: shipped v1.20 (compiled and tested offline; not yet run on the board).** Design and wire format in
DEVELOPER §22. Decisions on the open questions: (1) default legs `spec` 30 s -> `both` 40 s -> `ble` 20 s (`both`
covers the 5 GHz busy score at the cost of a longer leg); custom legs with `patrol <mode>:<sec>,...` (2-6 legs,
5-600 s) - leg configuration was not left for later. (2) On the dwell boundary (at most one dwell late). (3) Refused
both ways while a hunt or deauth runs (and while a USB/SD capture runs: v1 has no capture). Not persisted; a manual
`band` or BOOT walk stops it; the event log keeps running. Status: `pt` in hello and in a `{"t":"pt"}` line every
2 s in every mode (BLE has no dwells), the LCD header reads `patrol BOTH 23s`. Static RAM +56 B. Still open:
per-leg capture (the stretch below), and the on-board verify list.


**What.** A timer that walks the modes on its own — e.g. `spec` 30 s → `2.4g` 20 s → `ble` 20 s, repeat — so the
spectrum's unidentified-energy flag stays meaningful without manually sweeping the decode modes first (the 1.7 note:
one radio can't decode and energy-scan at once, so "known" is whatever they last saw).

**Why it fits.** Turns three separate features into a passive logger you leave on a desk — and it is the only
candidate that *improves an existing feature's accuracy* rather than adding surface.

**How (sketch).** A small state machine in the loop task: leg list [(mode, ms)], `patrol 0|1` command (leg
configuration later). Each hand-off is just `setBandMode()` — machinery exists; the catch is that every hand-off calls
`releaseCapture()`, so v1 patrols *without* capture (it is an analysis loop: device tables + spectrum bins), with
per-leg captures a documented stretch. Status: current leg + ms remaining in hello / the BLE heartbeat and the LCD
header next to the mode name.

**Open questions.** (1) Default legs — should `5g` get one too (the busy score lives there), at what cycle cost?
(2) Leg boundaries mid-dwell or on dwell boundary (cleaner stats, up to 220 ms late)? Recommend on-boundary.
(3) Patrol while hunt/deauth is active: probably refuse — both park the radio and last-writer-wins would be confusing.

**Verify.** Watch the mode walk in hello lines across a full cycle; per-leg sweep counters reset as today; free-heap
delta before/after each hand-off stays within a few hundred bytes (the capture-ring-release check from testing checklist 7).

### C6 — Top talker per dwell  ✅ SHIPPED v1.11

**Status (shipped, verified on-device).** `Accum` gained `bestMac[6]`/`bestRssi` (+8 B, one instance). `promiscuousCb`
tracks the loudest frame's transmitter; `sendDwell` reads it from `g_accum` (still live — `resetAccum` runs after)
into `"top"`/`"trssi"` on the `d` line, so no per-channel storage (would have been ~430 B across 54 channels). Serial
budget bumped 380→420. Host merges `top`/`trssi` (and keeps them across `s` sweep rows); the dashboard show a "Top
talker" column joined to the device table. **Verified:** parked on ch6, `top` was a stable MAC at −46 dBm each dwell.

**What.** The dwell already counts *how many* transmitters; name the loudest one. The `d` line gains `"top"`
(MAC of the strongest frame this dwell) + its RSSI → dashboard: "ch 6 is busy, and it's *this* Apple TV."

**How (sketch).** `Accum` gets best-mac[6] + best-rssi (+≈8 B; one instance under the existing `g_accumMux`) —
`promiscuousCb()` updates when a frame beats the current best, `finishDwell()` reads it into the line. BLE mode has no
dwells: skip (its 1 s heartbeat could carry it later). Host joins against the device table for name/vendor and shows
it on channel rows.

**Cost & constraints.** Serial budget: the dwell line grows ~25 B, so bump `serialRoom(380)` accordingly — hello's
budget was measured (900 then, 920 since v1.19) for exactly this reason (a line that passes the check then overruns is truncated mid-JSON). Everything
else happens inside locks already held per frame.

**Open questions.** (1) Top by peak RSSI in the dwell vs a smoothed value? Peak is cheaper and matches "who is on top now."
(2) Emit always or only when there was data (empty = null)?

**Verify.** Park on a channel with one strong AP — `top` should be its MAC every dwell. Add a second emitter, move it
closer/farther, watch the field follow.

### C7 — Hunt by SSID

**What.** `huntssid <name>` / `huntssid 0`: show live RSSI of every beacon carrying that name while hopping or parked —
"where's my mesh node" without knowing its MAC. Reuses PAGE_HUNT with the name as label.

**How (sketch).** A new hunt kind in the existing `Hunt` struct, with the name string heap-allocated only while active
(34 B) rather than resident — static budget is thin and a hunt is transient. Match in `trackWifiDevice()` on beacons
only (`flags & 1`, SSID compare under `g_devMux`) feeding the same `noteHuntHit()` path. Park: auto-derive from where
the strongest matching beacon was last seen, re-derived per sweep if it moves (MAC hunt parks only when given a channel).

**Open questions.** (1) Several APs share the name — show the best one's RSSI, list all in the dashboard row; fine?
(2) Compare semantics: plain `strcmp` after the existing sanitize (control chars stripped), case-sensitive.
(3) A 32-char label at hunt-page font size — v1.10 measured widths for exactly this kind of layout risk; re-check.

**Verify.** Two APs with one SSID on different channels: walk between them, RSSI and park follow the stronger.
A name that matches nothing → zero hits but no crash (the empty state).

### C8 — Least-busy readout  ✅ SHIPPED v1.11

**Status (shipped).** `quietestChannel()` (firmware, 0 static) picks the lowest `busyEma` among enabled/swept/non-rejected
channels; LCD Overview footer shows `quiet chNN N` (capture-off, where the line has width). The dashboard gained a
"Quietest channel" tile/slot, with an "nothing is quiet right now" caveat when even the minimum ≥ 50 (open question 1).
UI-only, no protocol change.

**What.** Overview shows the top‑3 *busiest*; add the quietest available channel ("quiet: ch149 · 3") for the
pick-a-channel use case. **UI-only**: every number is already in today's `s` rows (EMA per channel + state flag) —
no protocol change at all.

**How (sketch).** Host: min over channels with data and not rejected; LCD: one line under top‑3 on Overview (the v1.10
layout pass measured font widths for this page — verify it fits). Edge case: the quietest of an all-busy set is still busy
— suppress or caveat when its score crosses a threshold.

**Open questions.** (1) Suppress threshold value; (2) count only `hasData` channels — yes, a never-swept channel scores 0.

### C9 — Deauth refinements (rate + auto-stop on handshake)

**What.** Two small knobs for the attack: (a) **rate** — frames-per-tick is hardcoded to 4 in `serviceDeauth()`
(~33/s); a `deauthrate <1..16>` command makes both polite and aggressive reachable. (b) **auto-stop on handshake** —
count EAPOL frames - data frames (`(fc0 & 0x0C) == 0x08`) whose LLC/SNAP header carries ethertype `0x888E`; `fc0 & 0xF0 == 0xB0` is the *Authentication* management subtype, not EAPOL - while the attack runs; stop after N of them (default e.g. 4, or the
first one naming our target in `dca` mode). The README's "catch WPA2 handshakes" loop closes itself: the attack stops
when it got what it was for.

**How (sketch).** Both live where deauth already lives (`deauth_diag.cpp`) with one or two fields on the `d` line like
`da`/`df`. EAPOL counting is a few lines in `promiscuousCb()` gated on `deauth.active`; targeted mode can require the
target MAC as DA or SA for a stricter stop, broadcast counts any key-mgmt frame.

**Open questions.** (1) Auto-stop threshold N — and configurable? (2) In `dca` mode: first EAPOL naming the target is
enough evidence, four would be a full handshake; pick one and document it. (3) Does rate scale `txtest` too? Keep separate for now.

**Verify.** The witness sees the rate change directly (frames/s at the monitor). Auto-stop: leave an attack running against
an associated client that re-auths → `deauth auto-stopped (handshake)` log line, no host needed.

### C10 — Permit-join LED blip

**Status: shipped v1.19** with 1.6.1 as one decision (D1; `led_alert.cpp`, DEVELOPER §21). Purple double flash when a
node's permit-join bit first becomes set; the 802.15.4 ISR only sets `g_ledJoinFlag`. Open question (1) was settled the
cheap way: no per-node timestamp (0 B) - the sticky bit means a node blips **once** when first seen permitting, not
forever at beacon rate, so the 48 B fix was not needed. A node whose slot is evicted and recreated can blip again;
the 2 s rate limit covers it. Not yet verified with a real coordinator opening joins. Original entry below.

**What.** A Zigbee/Thread network setting its permit-join bit is a door opening — worth a blink. The flag already exists
per node (`Dev154.flags` bit2); add a rate-limited transient double-flash in `driveLed()`.

**How (sketch).** In the loop task, when any tracked node carries the bit and the last blip was > ~5 s ago: set a "blip
until" timestamp; `driveLed()` consumes it ahead of the busy-score colour. No new protocol line — the flag is already in
the `z` rows.

**Open questions.** (1) The bit is sticky-OR'd (`track154()` does `flags |= flagBits`) — a node that permitted join an hour
ago still blips forever. A per-node "last beacon-with-bit" timestamp (~1 B × 48 slots = 48 B) fixes it; recommend taking it.
(2) Which LED slot: rides the same transient proposal as 1.6.1's alerting — decide once for both (see C4).

**Verify.** A mains-powered Zigbee coordinator with permit-join toggled (its own LED usually blinks too): our blip follows
at roughly beacon rate, suppressed by the limit.

### C11 — Host CSV export

**What.** One button per device table → `bandwatch-wifi-devs.csv` download. Pure host: build rows from the state dicts
already in `/api/state`, Blob + download. Zero protocol change; feeds the "log this to a sheet" surveillance workflow and
makes cross-session diffs easy (which C4's events become, CSV vs CSV).

**Open questions.** Which columns — mirror the rendered tables? Include an RSSI summary (max/avg) over `hist`? Host-side decision.

### C12 — ESP-IDF port (the big swing)

Not a feature: the migration that unlocks features the pinned Arduino core cannot express. **What you gain:**
`CONFIG_BT_NIMBLE_EXT_ADV` (BLE 5 extended / coded PHY — the devices invisible today, ROADMAP known gap), re-tunable
`CONFIG_IEEE802154_RX_BUFFER_SIZE` (20 today), your own sdkconfig for stack sizes and the §16 budget questions, and §9's deauth
offsets stop being pinned to one exact core build — though they still need re-deriving, which is part of the cost. **What you lose:**
arduino-cli + the ctags wrapper simplicity (`setup.sh`, rule 12), the LVGL Arduino glue, and a known-good prebuilt blob.
**When it pays off:** when extended-adv-only environments (certain wearables/tags) matter more than build friction — i.e. after
C1–C11 are done and BLE coverage is the wall left. Everything else ships inside the current core; list this as the exit ramp, not a feature release.

**Suggested sequencing (to be argued with).** Batch one (**C3 + C6 + C8**) shipped in v1.11 — all small, no transmit duty, protocol cost
limited to a dwell field; banks RAM-headroom knowledge for the rest. Batch two: **C1** alone — the identity feature
(surveillance evidence), and its design questions deserve their own release. Then C2 (stage 1 before stage 2), C4 (shipped v1.18 ahead of C2;
its LED blip still waits on the shared decision with 1.6.1/C10), C5, then C7 and C9 as natural attachés of the hunt/deauth families. C11 rides any host
change; C12 when BLE coverage is the wall left.

---

## 1.13 — live LCD mirror (shipped, 1.13-1.14)

`mirror 1|0` streams repainted regions over serial; the v2 dashboard paints them and can step pages (§19).
Open: fast-updating pages (spectrum) tear - the 8 kB TX buffer and USB-serial bandwidth cannot keep up with
whole-page repaints, so drops trigger re-scans. Paging away is the practical answer today; a per-region
sequence number or lower mirror frame rate would be the next step if it matters.

## 1.12 — boot photos (shipped)

Eight pictures (`bootlogo/boot_*.png`, already panel-sized; sources in `bootlogo/orginals/`) are embedded as RGB565
arrays by `tools/img2c.py` -> `bandwatch/boot_logos.h`. A random one shows full-screen for 2 s at boot (then the mode
card), and the BOOT hold-walk gains a stop after Spectrum - release on it and the picture lingers 5 s while taps step
through them in order; (the page-wrap flash 1.12 added was removed in 1.12.2: pictures appear only at boot and on the walk's photo stop).

- **Flash, not RAM.** ~860 KB of const pixels in flash (app image now ~2.77 MB of a 3.14 MB partition, ~88%); LVGL draws
  from there, so the only RAM cost is the existing draw buffer. Regenerate after changing `bootlogo/`:
  `python3 tools/img2c.py bandwatch/boot_logos.h 172 320 bootlogo/boot_*.png`.

Open items:

- **Headroom.** ~370 KB of app partition left - a handful more pictures (or bigger ones) and the budget bites; a
  compressed format or palette pass would buy room if the set grows.

---

## 1.10 — mode splash (shipped)

A band change - button hold/walk, host command or boot - flashes a full-screen name card for the new mode before its
scan page takes over; holding BOOT walks the six cards at an even 700 ms cadence and release commits the one on
screen. Open items:

- **The splash is fire-and-forget.** It fades on its timer even if the picked radio (BLE/15.4) still needs a moment
  to come up; a brief "starting..." state would close that gap.

---

## 1.9 — dashboard v2 (shipped; classic retired in v1.19)

`dashboard2.html`: controls grouped into a sticky left rail, one scrolling column instead of five tabs, the active
radio's device table open while the other two fold into "last seen" caches, and hunt/deauth sharing a single action
bar. Same data, same zero‑dependency hand‑rolled charts; first served alongside the classic page (`--ui v2` /
`./host/run-v2.sh` flipped which sat at `/`; both removed in v1.19, see below). The SD pull flow landed with it: card
files stream back over serial (`sdread`) and become downloadable from the new UI.

Open items:

- ~~**Promote it.**~~ Done: v2 is the default at `/`.
- ~~**Retire classic.**~~ Done in v1.19: `host/dashboard.html` and `host/run-v2.sh` removed (git tag `v1.18.2` has
  the last copy), `/classic` 301-redirects to `/`, `--ui` is accepted and ignored.
- ~~**SD pull still stops at download.**~~ Now download + delete: every card file has a Download button (pull ->
  progress -> the browser saves it) and a two-click inline Delete (`sdrm`; DEVELOPER §12). Copies already pulled to
  the captures dir stay when the card file is deleted. Still open: opening a pulled pcap in Wireshark directly, or
  an in-page frame counter.

---

## 1.7 — spectrum analyzer (shipped)

A `spec` mode runs the 15.4 radio's `esp_ieee802154_energy_detect()` across channels 11–26 for a true
2.4 GHz RF-power reading (dBm, no decode), and the host flags per-bin energy that no recently-decoded
Wi-Fi/BLE/Zigbee emitter explains — evidence of an emitter whose protocol this radio can't demodulate, not
an identification. See [DEVELOPER.md §18](DEVELOPER.md). Known limits (the coarse one was addressed in 1.8):

- **2.4 GHz only.** The energy-detect primitive is a 15.4 feature; Wi-Fi/5 GHz expose no raw-energy API, so
  there is no swept analyzer above 2.4 GHz without moving off the prebuilt core.
- **Coarse resolution — addressed in 1.8.** Was 16 fixed 5 MHz bins (the 15.4 channel grid); v1.8 sweeps off-grid
  at a selectable 1/2/5 MHz step (`specstep`). Sub-MHz swept-centre-frequency resolution would still need driver
  work and is unverified.
- **Correlation is time-separated.** One radio can't decode and energy-scan at once, so "unexplained" is
  relative to the last Wi-Fi/BLE/15.4 sweep. C5 patrol mode (v1.20) keeps that sweep recent automatically.

---

## 1.6 — untethered surveillance detection

**Goal:** make the detection added in 1.5 useful with no laptop attached. Detection already runs on the
device (`kSurvOuis` is matched in firmware precisely so the LCD can flag without a host); what is missing is
everything around it.

### 1.6.1 Alerting — *highest value, smallest change*  ✅ shipped v1.19

**Status:** the transient double-flash below was agreed (D1) and shipped with C10 and the C4 novelty blip
(`led_alert.cpp`, DEVELOPER §21): a surveillance device's first slot this session gives an orange double flash, with
or without the event log armed; it may override hunt/record/busy, never an active deauth (alerts are dropped during
one); at most one blip per 2 s, highest pending kind wins. `alerts 1|0` (persisted) and `ledtest surv|new|join`.
Original entry:

A marker on a page you have to be looking at is not an alert. Needs an active signal when a surveillance OUI
is matched.

The board has **no buzzer**, so the RGB LED is the only channel, and it is already four-way contended in
`driveLed()`: deauth blink > hunt distance > record pulse > busy score. A fifth state needs a slot in that
hierarchy, and the awkward case is real — recording *and* detecting at once is exactly when you are walking
around with the device in your hand.

Proposed: a detection wins the LED briefly (distinct fast double-flash, a few seconds) then yields back to
whatever was showing, rather than becoming a new permanent state. Not yet agreed.

### 1.6.2 Hit logging to SD  ✅ delivered by C4 (v1.18)

Delivered as `/events.csv` (C4, DEVELOPER §20): MAC, category, tier, RSSI, channel and epoch/uptime timestamps, plus
novelty rows. Original entry:

Untethered means nobody is watching the dashboard, so hits must persist: MAC, category, tier, RSSI, channel,
timestamp, appended to a file on the card. This is the actual deliverable of a walk-around.

No longer RAM-blocked: static was ~74 kB at v1.15.4, ~6 kB under the line, so a ~512 B in-RAM ring fit (77,392 B
and under 3 kB of headroom since v1.19). The
concrete design is candidate C4 (events.csv + novelty baseline).

### 1.6.3 Control without a host

`sdcap`, `time` and band/park are all host commands today. Recording-start was deliberately made
host-only in 1.3 (the alternative was explicitly considered and rejected then), but untethered use changes
that trade-off. Needs a BOOT-button path, and a decision about the LCD page/long-press mapping, which is
already carrying "tap = page, hold = walk the mode cards (photo stop after Spectrum), release = commit; hold on
Hunt = stop hunt, then walk".

Also unresolved untethered: the device has no RTC, so with no host to send `time <epoch>` the hit log and
pcap filenames fall back to a counter and uptime-based timestamps.

### 1.6.4 OUI list on the SD card  ✅ delivered by C4 (v1.18)

Delivered: `/surveil.csv`, up to 64 lines of `AA:BB:CC,<category 1-7>`, read once at boot into heap; the built-in
table is checked first, so the file can add prefixes but not withdraw a built-in one (that still needs a reflash).
Original entry:

A 64-prefix table goes stale, upstream already withdrew two Flock prefixes as Ubiquiti false positives, and
reflashing in the field is not an option. Reading `/surveil.csv` at mount and falling back to the built-in
table would make the list updatable by dropping a file on the card. Folds into C4.

---

## Memory budget — replace the proxy with a measurement

`CLAUDE.md` rule 4 says static RAM must stay "well under ~80 KB". Origin: commit `265d955` (1.2), after real
OOM crashes — `abort() … lock_init_generic` (newlib could not allocate a mutex) and a store fault in
`lv_obj_class_create_obj` (LVGL `malloc` returned NULL). Someone cut static RAM, the crashes stopped, and the
line stuck. **It is an empirical heuristic, not a derived limit, and nobody has re-derived it since 1.2.**

The mechanism behind it is real: static allocations and the heap share one 320 KB DRAM pool, so every static
byte costs a heap byte 1:1. Note the build output is actively misleading here — it reports *"leaving 248104
bytes for local variables"*, but measured free heap at runtime is ~99-106 kB in `both` depending on the LCD page (v1.15.5) and ~88 kB in BLE, because the Wi-Fi/BLE driver stacks
and FreeRTOS task stacks take the rest.

What actually binds is **free heap at peak concurrent load**. Measured on hardware:

| state | free heap |
| --- | --- |
| idle, Wi-Fi `both` (v1.15.5: Overview / System / Devices / Channels) | 98.9 / 102.3 / 106.4 / 106.4 kB |
| idle, BLE (v1.15.2, 2026-10 audit) | ~88 kB |
| event log armed + USB + SD capture, every page (v1.19) | **25.2 kB** (Overview; floor 24 kB) |
| USB + SD capture, any page (v1.15.5) | ~27.8 kB (worst on Overview; full 20-slot ring) |
| USB + SD capture, any page, any order (v1.15.4) | ~30.8 kB (worst on Overview; 11 ring slots) |
| `cap 1` then `sdcap 1` before 1.15.3 | 16.1 kB |
| capture started on Channels, then step to Overview (1.15.3, before page headroom) | 16.4 kB |
| BLE + SD capture (pre-1.11, not re-measured) | 32.3 kB |
| SD capture with the pre-1.3 ring ordering | 12.5 kB — where LVGL page rebuilds start failing |

Done in this review pass: `ensureCapRing()` (`capture.cpp`) checks total free heap against `kMinFreeHeapB`
(24 kB) right after allocating the ring and *refuses* capture with a JSON error when it falls below — checkable
at runtime, tied to the actual failure mode (worst concurrent case: capture ring + FATFS + an LVGL page rebuild
landing together). The static-RAM rule stays as the backstop for everything else; under such a rule the 512-byte
hit log stops being a rule violation. Since 1.15.3 it *sizes* the ring down toward `kCapSlotsMin` to keep the
floor instead of refusing, and `sdcap` re-fits a ring that `cap 1` made before the FATFS mount (`refitCapRing()`);
since 1.15.4 the floor also includes `lcdPageHeadroomB()` - each page records the heap it took to build, and the
ring leaves room to step to the heaviest page available in the current mode.

Current static usage is **77,184 B** (v1.19.3; 77,392 B at v1.19; 74,032 B at v1.15.4, 79,592 B at 1.5.5 and 80,544 B at 1.10 before
the v1.11 reclaim - smaller LVGL buffer + DevRef listing path). Under 3 kB below the line, which is still an
unverified budget, not a wall; `tests/firmware/static_ram_ceiling.json` gates it.

1.5.4 is a worked example of the squeeze it causes: adding a station's BSSID to `WifiDev` should have been
6 bytes, but that struct is in two `kWifiDevSlots` arrays and would have rounded 64 → 68, i.e. 512 B, which
does not fit. Repacking the struct to exactly 64 bytes bought 3 bytes of former padding for free instead — a
good outcome here, but the reason only 3 of the 6 bytes are on the device is this unverified line, not
anything about the radio. (Since v1.11 the DevSnap copy is gone and `WifiDev` lives in one array, so the same
rounding would now cost 256 B.)

**Done in v1.15.4: the Channels LCD page.** The 2026-10 RAM audit measured free heap per LCD page in `both` band:
System ≈ 101.4 kB, Devices ≈ 90.3, Overview ≈ 86.8, **Channels ≈ 55.8** - its 39 rows of row + 2 labels + bar were
~156 LVGL objects. v1.15.4 draws the grid as one object (`chanGridDraw()` on `LV_EVENT_DRAW_MAIN_END`, a 3-byte
`ChanCell` snapshot per row, only changed cells invalidated): Channels now sits at **≈106.3 kB** (+50 kB) and builds
in ~1.9 kB, making it the lightest page. That inverted the risk - a capture sized on Channels went under the floor on
the next step to Overview (16.4 kB) - hence `lcdPageHeadroomB()`. v1.15.5 custom-drew Overview's
channel strip and the Devices list too (+11.3 / +15.2 kB on those pages; the five pages now span ~99-106 kB). v1.16.1
did the Spectrum page's 42 bars the same way (shared `BarStrip`), which also fixed a layout bug: 42 × 3 px bars on a
2 px gap needed 208 px in a ~156 px panel, so the top three bars (~2477-2483 MHz) were clipped off-screen. Parked from the audit,
not worth their risk at this headroom: heap `specFine[]` only in spec (504 B, null guards in five readers), `buf1`
`/21` → `/30` (~1.6 kB, flicker risk), `Deauth.slotPad` (§9 territory - leave).

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

- ~~**`sdls` can silently truncate.**~~ Fixed in 1.5.1: the reply carries `total` and `sent`, and the host
  shows the card's file list knowing whether it is complete. (1.5.4 fixed the same gap independently with a
  `listed`/`skipped` pair that logged a text line instead; the merge in 1.5.5 kept `total`/`sent`.)
- **Surveillance detection is unproven against real hardware.** The matcher is unit-tested against known
  values from both upstream sources plus a negative control, but no Ring/Flock/Axon device has ever been in
  range during testing. The first live hit is the real test.
- **BLE 5 extended advertising is invisible.** `CONFIG_BT_NIMBLE_EXT_ADV` is not set in the fixed sdkconfig,
  so devices advertising only on extended or coded PHY do not appear at all. Lifting this means moving the
  project to ESP-IDF (rule 7).
- **Device names are environment-capped, not code-capped.** Measured: 31 distinct advertisers, 3 ever
  broadcast a name. Most nearby traffic is Apple continuity and non-connectable beacons that never answer a
  `SCAN_REQ`. No scanning strategy changes that.
- **Whether a deauth actually disconnects a real station is untested.** 1.6 established that correct deauth
  frames reach the air at the target; it did not establish that any particular client honours them. That
  needs a device you own, associated to an AP you own. Unprotected-frame handling varies by supplicant, and
  PMF-enabled networks ignore these frames by design. [`DEVELOPER.md`](DEVELOPER.md) §11.

---

## Blocked

Nothing, as of 1.15.4. Both entries that stood here are resolved; they are kept struck through because the
conclusions reversed.

- ~~**Deauth does not work and the root cause is unidentified.**~~ Resolved in 1.6 by building the second
  radio this entry asked for: `tools/witness/` is an ESP32-S3 in monitor mode plus `verify.py`, and every
  claim about the attack is now an observation from it rather than from the device's own counters. Two
  unrelated faults had been hiding each other — the raw `esp_wifi_80211_tx()` path was rejected before TX by
  a subtype gate inside the closed-source `libnet80211.a` (`ESP_ERR_INVALID_ARG`, 304 of 304), and the
  driver-internal slot path radiates nothing for *any* subtype, so the §9 offsets are wrong rather than
  merely fragile. Raw TX is now the default and `tools/deauth/patch_raw_tx.py` patches the gate out between
  compile and flash (`build.sh` does this by default; `--no-patch` opts out). 289 deauth frames witnessed on
  air at -38 dBm. Full log in [`DEVELOPER.md`](DEVELOPER.md) §11.
- ~~**SoftAP injection PoC is inconclusive**, not negative.~~ Its premise was disproven in 1.6: raw TX does
  radiate from an unassociated STA — `txtest` injects from `WIFI_IF_STA` with no SoftAP up and was witnessed
  at -45 dBm. The PoC was not a route to anything, and the `softap` command was removed in the 1.6 review pass.

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
  `grep -q "\"$(git describe --tags --abbrev=0 | tr -d v)\"" bandwatch/bandwatch_core.h`
  (`kVersion` moved out of `bandwatch.cpp` in the 1.5.1 split).
- **A feature was shipped, tagged and documented without ever being run.** The `dca` dashboard path was
  broken in four independent places at once — a filter on a field that did not exist, buttons with no way to
  show them, a MAC format the device rejects, and an unhandled acknowledgement. Each one alone makes the
  feature do nothing. Reaching a new control *through the UI it ships with*, once, would have caught all four.
