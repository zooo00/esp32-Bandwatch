# Bandwatch roadmap and open items

Planned work, open questions and known gaps. Current release: **v1.10**.

Entries say what is actually known, including what has *not* been verified. Anything measured is quoted
with its numbers; anything assumed is labelled as such.

---

## 1.11 — candidate features (draft)

Twelve candidates collected for analysis. They are independent unless noted, and each is sized to ship
as its own release or in small batches. Written so an analyst who has not seen the code can evaluate:
costs name where they come from, and anything that needs a decision sits under *Open questions* instead
of being assumed.

**How to analyze.** Rank by value-for-effort against this project's identity (portable Wi‑Fi/BLE/15.4
surveillance + interference tool; one time-shared radio). Check the RAM math in the preamble — static
headroom is thin now. Anything adding a protocol line costs a three-way sync (firmware sender ↔ host
`merge_*` in `bandwatch_host.py` ↔ the §4 table in DEVELOPER.md); UI-only candidates skip that cost.
The quick hits (C6–C11) are cheap enough to batch; the big ones each justify their own release.

**Preamble — constraints any candidate must respect:**

- **One radio, six modes.** `5g / 2.4g / both / ble / 154 / spec` are exclusive; `setBandMode()`
  (`bandwatch.cpp`) is the only swap point and calls `releaseCapture()` on every change (pcap link type
  differs per radio). New mode = extend `BandMode`, `kBandName[]`, `chanEnabled()`, `setBandMode()`,
  LCD pages, `sendHello`'s `chs`, dashboard buttons (checklist in DEVELOPER §6).
- **RAM is the constraint.** No PSRAM; statics and heap share ~320 kB DRAM. Static usage measured on a
  clean v1.10 build: **80,544 B** — *over* the round "well under ~80 kB" line (CLAUDE.md rule 4), up from
  79,592 B at v1.5.5. Free heap at runtime: Wi‑Fi idle ≈83 kB, BLE ≈95 kB, SD capture (tightest) **31.9 kB**;
  `ensureCapRing()` refuses a ring that leaves < `kMinFreeHeapB` = 24 kB total free. `WifiDev` is packed to
  exactly 64 B behind a `static_assert` (`devices.h`) and lives in two 64-slot arrays, so +1 byte of *padding*
  can cost 512 B static. For reference: `BleDev` ≈56 B × 48 slots (hand-computed — add a static_assert before
  relying on it), `Dev154` = 24 B × 48, `CapFrame` = 1,610 B × up to 20.
- **Three contexts + one ISR.** Loop task (LVGL timer + `Bandwatch_Loop`, ~2 ms cadence), Wi‑Fi task
  (`promiscuousCb` in `wifi_sniff.cpp`), NimBLE host task (`bleGapEvent` in `ble_scan.cpp`), and the 802.15.4
  true ISR. Radio paths are `IRAM_ATTR`, spinlocked, no heap / no Serial. SD writes and capture draining run
  on the loop task only — a radio-side event must flag-then-drain (existing pattern: `s_edReady` in
  `ieee154.cpp`, `bleScan.activeUntilMs`).
- **Serial never blocks.** 8 kB TX buffer, `setTxTimeoutMs(0)`, every line checks `serialRoom()` and is
  dropped whole. Current budgets: hello 900 (measured), dwell 380, sweep 1500 (fine-spectrum up to ~1.7 kB),
  Wi‑Fi device rows `40 + n·126` (a full 64-device table ≈ 7–8 kB of the buffer). New per-line fields grow these.
- **No RTC.** Timestamps come from the host's `time <epoch>`; without it, uptime-based times and counter-named
  files. Anything persisting to SD inherits this.
- **Anything that transmits needs a dead-man's switch** (`kDeauthMaxMs` = 5 min auto-stop) — there is no other
  way for the board to stop itself. Attacks also raise TX power (82 → 160, i.e. 8.2 → 16 dBm).
- **LED priority in `driveLed()`** (`lcd_ui.cpp`): deauth blink > hunt distance > record pulse > busy score.
  A new *permanent* state needs a slot; the 1.6.1 alerting item is still undecided — candidates adding LED
  behaviour should say which slot they take or use a transient blip that yields back.
- **SD shares the LCD's SPI bus** (CS GPIO4, 20 MHz), loop-task only, budgeted at `kSdBudgetUs` = 8 ms per
  loop; mounting FATFS costs ~30 kB and is done on demand (`sdMount()`/`sdUnmount()`, §12).

| # | Name | One-liner | Static RAM cost | New protocol lines? | Est. effort |
|---|------|-----------|-----------------|---------------------|-------------|
| C1 | Probe-request mapping ("seeking") | sleepy devices reveal which network they want | ~0.2–0.4 kB (dedup table) | yes: `pr` events | S–M |
| C2 | Ghost AP mode (+ probe responder) | attract sleepers, catch their handshakes | <100 B + heap scratch while active | yes: `ghost` cmd/ack, counters in `d` | M |
| C3 | NVS persistence | boot where you left off; settings survive reboot | transient ~0.5–1 kB, none resident | no (maybe a log line) | S |
| C4 | Event log → SD CSV (+ novelty baseline) | persistent hits + "new device here" without a host | pending queue ≈200 B | no (an SD file) | M |
| C5 | Patrol mode (auto round-robin) | keeps the spectrum's "known emitters" fresh; passive logging | timer state, few bytes | yes: `patrol` cmd/ack, leg in status lines | S–M |
| C6 | Top talker per dwell | name the loudest voice on each channel | +≈8 B in `Accum` (one instance) | field added to `d` line | S |
| C7 | Hunt by SSID | "where's my network" without knowing its MAC | 34 B while active, heap-allocated | reuses `hunt` ack shape; new command | S–M |
| C8 | Least-busy readout | quietest channel on Overview / dashboard | 0 — computable from today's `s` rows | no | S (UI only) |
| C9 | Deauth refinements | rate control + auto-stop when the handshake is caught | a few bytes | fields in `d`, new command | S–M |
| C10 | Permit-join LED blip | a Zigbee door opening = brief double-flash | 0 (or +48 B, see entry) | no | S |
| C11 | Host CSV export | one-click device-table download | n/a (host side) | no | S (UI only) |
| C12 | ESP-IDF port | unlocks BLE 5 extended adv + re-tunable driver config | re-derive §9 offsets and the §16 budget | wire format unchanged | L (a migration, not a feature) |

### C1 — Probe-request mapping ("seeking" SSIDs)

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
last-writer-wins dance as hunt/deauth), raise TX power to 160 like `startDeauth()`, and re-send a beacon from the
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

### C3 — NVS persistence (boot where you left off)

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

### C4 — Event log to SD CSV (+ novelty baseline)

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

### C5 — Patrol mode (auto round-robin)

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

### C6 — Top talker per dwell

**What.** The dwell already counts *how many* transmitters; name the loudest one. The `d` line gains `"top"`
(MAC of the strongest frame this dwell) + its RSSI → dashboard: "ch 6 is busy, and it's *this* Apple TV."

**How (sketch).** `Accum` gets best-mac[6] + best-rssi (+≈8 B; one instance under the existing `g_accumMux`) —
`promiscuousCb()` updates when a frame beats the current best, `finishDwell()` reads it into the line. BLE mode has no
dwells: skip (its 1 s heartbeat could carry it later). Host joins against the device table for name/vendor and shows
it on channel rows.

**Cost & constraints.** Serial budget: the dwell line grows ~25 B, so bump `serialRoom(380)` accordingly — hello's
900 was measured for exactly this reason (a line that passes the check then overruns is truncated mid-JSON). Everything
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

### C8 — Least-busy readout

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
count key-management frames (`fc0 & 0xF0 == 0xB0`) while the attack runs; stop after N of them (default e.g. 4, or the
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
C1–C11 are done and BLE coverage is the wall left. Everything else ships inside the current core; list this as the exit ramp, not 1.11.

**Suggested sequencing (to be argued with).** Batch one: **C3 + C6 + C8** — all small, no transmit duty, protocol cost
limited to a dwell field; banks RAM-headroom knowledge for the rest. Batch two: **C1** alone — the identity feature
(surveillance evidence), and its design questions deserve their own release. Then C2 (stage 1 before stage 2), C4 (with the
shared LED decision from 1.6.1/C10), C5, then C7 and C9 as natural attachés of the hunt/deauth families. C11 rides any host
change; C12 when BLE coverage is the wall left.

---

## 1.10 — mode splash (shipped)

A band change - button hold/walk, host command or boot - flashes a full-screen name card for the new mode before its
scan page takes over; holding BOOT walks the six cards at an even 700 ms cadence and release commits the one on
screen. Open items:

- **The splash is fire-and-forget.** It fades on its timer even if the picked radio (BLE/15.4) still needs a moment
  to come up; a brief "starting..." state would close that gap.

---

## 1.9 — dashboard v2 (shipped, running in parallel)

`dashboard2.html`: controls grouped into a sticky left rail, one scrolling column instead of five tabs, the active
radio's device table open while the other two fold into "last seen" caches, and hunt/deauth sharing a single action
bar. Same data, same zero‑dependency hand‑rolled charts; served alongside the classic page — `--ui v2` /
`./host/run-v2.sh` flips which one sits at `/`, and both pages cross‑link. The SD pull flow landed with it: card
files stream back over serial (`sdread`) and become downloadable from the new UI.

Open items:

- **Promote it.** Once v2 has lived with you for a while, make it the default route and drop the classic page (and
  its cross‑links) rather than carrying both forever.
- **SD pull still stops at download.** One step away from opening the pulled pcap in Wireshark directly; a tiny
  "open" affordance or an in‑page frame counter would close that gap.

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
  relative to the last Wi-Fi/BLE/15.4 sweep. An automatic round-robin (dwell in `spec`, periodically dip into
  the decode modes to refresh the "known" picture) is a possible future improvement.

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

Done in this review pass: `ensureCapRing()` (`capture.cpp`) checks total free heap against `kMinFreeHeapB`
(24 kB) right after allocating the ring and *refuses* capture with a JSON error when it falls below — checkable
at runtime, tied to the actual failure mode (worst concurrent case: capture ring + FATFS + an LVGL page rebuild
landing together). The static-RAM rule stays as the backstop for everything else; under such a rule the 512-byte
hit log stops being a rule violation.

Current static usage is 79,592 B — about 408 B under the existing line. (Measured on the merged 1.5.5 tree;
the two branches it came from were 79,560 B at 1.5.2 and 79,584 B at 1.5.4, so carrying both feature sets
cost 8 B over the larger of them.) That headroom is the edge of an unverified budget, not a wall.

1.5.4 is a worked example of the squeeze it causes: adding a station's BSSID to `WifiDev` should have been
6 bytes, but that struct is in two `kWifiDevSlots` arrays and would have rounded 64 → 68, i.e. 512 B, which
does not fit. Repacking the struct to exactly 64 bytes bought 3 bytes of former padding for free instead — a
good outcome here, but the reason only 3 of the 6 bytes are on the device is this unverified line, not
anything about the radio.

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

Nothing, as of 1.6. Both entries that stood here are resolved; they are kept struck through because the
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
