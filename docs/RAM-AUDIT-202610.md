# Bandwatch RAM & leak audit — findings + implementation plan (2026-10)

> **Status (v1.15.3):** F1, F2 (option a), F3 (BLE only), F4 comment, F5 note and the F6 indev removal shipped in
> 1.15.3 — see `CLAUDE.md` history and `DEVELOPER.md` §16. Two corrections from review: F1's suggested
> `releaseCapture()` would also close the SD file and stop USB capture (shipped as `refitCapRing()` instead), and
> F3's Wi-Fi bump to 140 would exceed the 8 KB TX buffer at 64 rows (left at 126). F3's "drops whole" is also
> wrong: an under-budget line truncates mid-write. Remaining items (the ~45 kB Channels page, other F6
> candidates) are in `ROADMAP.md` under *Memory budget*.

Audit of the firmware (`bandwatch/`) plus host tool, focused on memory leaks and RAM savings. All findings are
**verified against commit `c0b2bd7` (v1.15.2)** on a plugged-in Waveshare ESP32-C5-LCD-1.47; no fixes implemented yet.
Anything measured is quoted with its number; anything assumed is labelled as such. Written so an implementer who has
not seen the code can act on it.

Board facts driving the calls: 320 KB SRAM, no PSRAM; static DRAM in this build = **74.5 kB** of `.dram0` sections
(the linker reports ~79.6 kB — §16's "under ~80 kB" line has ~400 B–5 kB headroom depending on which arithmetic you
trust). Free heap at idle, measured: Wi-Fi `both` ≈ 83–87 kB (page-dependent, see F5), BLE ≈ 88 kB.

**Re-measuring:** board serial `/dev/cu.usbmodemXXXX` @ 115200; free heap rides on every dwell line (`s`), BLE
heartbeat (`ble`) and fine-sweep lines (`fs`). Commands: `info`, `band both|ble|spec`, `page next/prev`, `reboot`.
Probe scripts used for this audit live in `tools/` (port from `BANDWATCH_PORT`, default `/dev/cu.usbmodem1101`):

- `probe_ble.py <targetPage> [rounds]` — reboot into `both` (BT cold), sample W0 → first BLE visit → back, on one
  fixed page. Pages: 0 overview, 1 channels, 3 devices, 5 system.
- `probe_pages.py` — per-page free heap in `both`, repeated passes.

---

## Findings (ranked)

### F1 · RAM hole: `cap 1` then `sdcap 1` defeats the heap-floor guard — *real OOM-regime risk*

The floor (`kMinFreeHeapB` = 24 kB, `capture.cpp:11`) is only validated against the heap **at ring allocation**.
Only the sdcap-first ordering is protected (it mounts FATFS before sizing; comment at `host_proto.cpp:328`). Cap-first does this from Wi-Fi idle (~83 kB):

| step | free heap |
| --- | --- |
| `cap 1` → ring sized to full 20 slots = **32.2 kB** (`CapFrame` = 1610 B × `kCapSlotsMax` 20, `capture.cpp:34-38`) | ~51 kB — floor check passes |
| `sdcap 1` → FATFS mount (−~30 kB, `sd_sink.cpp:39`) + SD buffer malloc (−4 kB); ring already committed, no re-check | **~17 kB — below the 24 kB it promised** |

The dashboard has both buttons independently, so this is reachable by click order. F5 shows page state swings free
heap another ±45 kB, so on the heavy CHANNELS page you can land near §16's old "12.5 kB, LVGL page rebuilds start
failing" row — exactly the regime the floor was added to prevent (the 1.3 regression).

**Fix (~10 lines, firmware):** in the `sdcap` handler (`host_proto.cpp:327-341`), when a ring already exists and
FATFS wasn't mounted yet: after successful `sdOpenCapture()`, re-validate `ESP.getFreeHeap() >= kMinFreeHeapB`; on
failure → `releaseCapture()` then `ensureCapRing()` (now sized against post-mount heap); if that fails too,
`sdCloseCapture()` + the existing err ack. Restores the invariant for every ordering. Alternatives considered: host
sends sdcap before cap (works but leaves raw-serial users exposed); bumping `kMinFreeHeapB` shrinks the ring in the
*good* ordering too.

**Verify:** idle `both`, send `cap 1` then `sdcap 1`, watch the `heap` field dip below 24 kB (it can't before this
fix when sdcap goes first). Repeat with order swapped — both should now sit at roughly the same floor.

### F2 · Park in spec mode: acked and displayed, then silently cleared on band-leave — *behavior bug*

Three layers; the third is the real one:

1. `advanceChannel()`'s spec branch (`bandwatch.cpp:114`) walks the fine bins without consulting `parkedIdx`, so a
   park set in spec mode doesn't affect the sweep. The v2 dashboard knows and shows a note for this.
2. **`setBandMode()` clears any park whose channel isn't enabled in the new mode** (`bandwatch.cpp:167`). A park set
   before entering spec (or in it, as a 15.4 ch) is wiped on entry/exit to spec or 15.4 — so the v2 dashboard note
   *"parking applies once you switch back to a hopping mode"* (`dashboard2.html:547`) is false; there's nothing left
   to apply. (v1 hides the park control in non-Wi-Fi bands, `dashboard.html:539`, so this is mostly v2 + raw-serial.)
3. `host_proto.cpp:398` already treats park as intentionally not persisted ("park is intentionally not saved") —
   extending that to band-switches would be consistent if the note stopped overpromising.

**Fix options:**
(a) *Honor it in spec* (~6 lines): in the spec branch of `advanceChannel()`, when `parkedIdx >= 0` jump to the bin for
    that channel — `mhz = ch154Freq(kChannels[parkedIdx])`, `bin = (mhz − kSpecLoMhz) / specStepMhz` clamped to
    `[0, specBinCount())`; makes ack, LCD and note all true. Note: in spec mode only 15.4 channels can be parked
    (`chanEnabled` = `is154`, `bandwatch_core.h:282`).
(b) *Keep it per-mode*: change the v2 note to "parking resets when you enter/leave the spectrum".

Recommend (a); it's small and makes every surface agree.

**Verify:** spec band, `park 24`, watch the fine sweep (`fd` lines / LCD header MHz) — with (a) it holds on ~2465
MHz; without, it keeps walking. Also: park ch6 in `both`, go to `spec`, back to `both` → `hello`'s park field should
still say 6 (true only if the clear at `bandwatch.cpp:167` is reworked for spec-set parks).

### F3 · BLE device-row serial budget: worst case ~104–105 B vs 102 — *dropped refreshes in a corner*

`sendDevices()` budgets `serialRoom(40 + n * 102)` per BLE row (`host_proto.cpp:273`). Re-derived from the `BleDev`
field types (`devices.h:43`) for one maxed row (commas included):

```
[mac"           20    "aa:bb:cc:dd:ee:ff" in quotes
,rssi −128       5     int8
,maxRssi −128    5     int8
,adv 65535       6     uint16
,age ≤ 60000     6     kDevFreshMs = 60 s (bandwatch_core.h:36)
,addrType        2
,company 65535   6     uint16 (SIG ids are already 4–5 digits)
,"name×20"       22    char[21], before JSON escaping — a quote in the name adds more
,appearance      6     uint16
,txPower −128    5     int8
,svc / ,svcData 12     two uint16
,appleType 255   4
,flags / ,surv   4
]                1
                  ─── ≈ 104 B (≈ 105–120 with escaped name chars)
```

Overrun drops the **whole** `b` line per cycle (drop-whole policy, no corruption) — a missing BLE-table refresh
whenever a corner-case device coexists with enough other rows. The Wi-Fi row is similarly borderline: fits at ~126/126
(`host_proto.cpp:251`) until a non-printable country byte escapes to `\uXXXX` (then ~133; rare — most are 2-letter ISO).

**Fix:** bump the per-row budget 102 → **~116**, and 126 → ~140 if you want cc-escape safety. One line each.
(Cheapest real fix in this audit — do it first.)

### F4 · NimBLE `deinit(false)` retention ≈ 250 B — *not the "10 KB class"; probably no change*

Measured (reboot → `both` with BT cold → first `band ble` visit → back to `both`, same page both times):
**Δmin = +260 B after the first visit, +16 B on the second** (noise floor ±~100). Why so small: the controller's
*static* DRAM in this build is only ~62 B (`libbt.a`+`libble_app.a` contributions to `.dram0` in `bandwatch.ino.map`),
and everything else — NimBLE host task stack 5.1 kB, msys pools ~10.7 kB (sdkconfig: MSYS_1 24×128 + MSYS_2 24×320),
transport buffers — returns to the heap via `nimble_port_deinit()`, which `BLEDevice::deinit()` calls regardless of the
flag (`stopBle()` at `ble_scan.cpp:210`).

The reason to keep `deinit(false)` is **not RAM**: core 3.3.11's `BLEDevice::init()` refuses when
`btMemReleased(BT_MODE_BLE)` (verified in `~/.arduino15/.../libraries/BLE/src/BLEDevice.cpp`, init guard ~line 298), so
switching to `deinit(true)` would save ~0.25 kB but break re-visiting `band ble` without extra re-init handling.

**Fix:** none now — optionally add a comment at `ble_scan.cpp:213` with the measured number, plus a ROADMAP note to
re-check if the C5 BT config grows. (This corrects an earlier "~10 KB permanent" guess that predated the measurement.)

### F5 · LVGL page choice swings free heap by ~45 kB — *context for §16's floor*

Measured on one board, `both` band, repeated passes (stable to ±0.1 kB):

| page | free heap |
| --- | --- |
| SYSTEM | ≈ 101.4 kB |
| DEVICES | ≈ 90.3 kB |
| OVERVIEW | ≈ 86.8 kB |
| CHANNELS (heaviest — 39 rows × row/label/bar) | **≈ 55.8 kB** |

The tightest normal state is therefore not just *SD capture* but *SD capture while sitting on the Channels page*.
Reinforces F1.

**Fix:** none in code; add a line to DEVELOPER.md §16's table (which page its numbers were taken on + this spread).

### F6 · Static-RAM savings candidates (~1–2 kB total; buys headroom under the ~80 kB line)

Take as needed, each is independent:

| item | size | note |
| --- | --- | --- |
| `specFine[84]` (`bandwatch_core.h:188`) | **504 B** (`.data`, initialized) | only used in spec mode → heap it at `startSpectrum()` / free at `stopSpectrum()`. Cleanest win. |
| LVGL draw buffer `buf1` (`LVGL_Driver.cpp:13`; len = `W×H/21`) | **5,240 B** → ~3,670 at `/30` | saves ~1.6 kB; the SPI flush is synchronous (single buffer already), so smaller just means more flush passes per page rebuild — needs a UI feel-check. |
| `Deauth.slotPad[256]` (`bandwatch_core.h:235`) | 256 of the 288 B struct | only the dead internal-kick path (`kickpath 1`) writes it, and its comment says the driver touches `&flag+210`: shrink to ~224 saves 32 B, or lazy-alloc on kickpath=1 saves ~250 B. Keep whatever you pick working with the §9 offset work in mind. |
| Orphaned POINTER indev (`LVGL_Driver.cpp:54`) | ~200–300 B heap (estimated) | created with an empty read callback ("No touch input"); the BOOT button is a GPIO handled at `lcd_ui.cpp:1063`. Nothing else references it — removable if no LVGL feature needs *an* indev to exist. |

---

## Checked and OK (skip re-auditing these)

- **Host capture lifecycle** (`bandwatch_host.py`): start guarded by `self.pcap`; `handle_frame()` (~line 1007)
  snapshots the local ref before the HTTP thread can close it — worst case one frame dropped inside a caught exception;
  `PcapWriter` (line 284+) writes direct per frame under its lock, no unbounded buffering. `_sd_read` reassembly
  bounded by file size, closed on ack or err line.
- **Dashboard JS** (`dashboard.html`, `dashboard2.html`): single self-rescheduling tick each (no interval stacking),
  toasts remove themselves at ~4.5 s, mirror `ImageData` reused, listeners on stable elements + document delegation,
  `picks[]` capped at 6.
- **ST7789 driver** (`Display_ST7789.cpp`): zero dynamic allocations — already uses `SPI.writeBytes()` specifically to
  avoid a stack read buffer (commented). Only nit: ~13 begin/endTransaction per flush; perf, not memory.
- **`surv_ouis.h`**: constexpr table in flash (~200 B rodata), no DRAM cost.
- **BLE re-init with `deinit(false)`** works at runtime: second visit measured identical heap and scanning fine.

## Suggested implementation order / batching

1. **F3** — one-line budget bumps (102→116, optionally 126→140). Cheapest real fix; ships alone or with anything.
2. **F1** — sdcap floor re-check in `host_proto.cpp`. The only true OOM-regime risk.
3. **F2** — pick semantics (honor-in-spec recommended, ~6 lines + the v2 note string).
4. **F5 note + F4 comment** — docs-only; fold into whichever release touches §16 / `ble_scan.cpp`.
5. **F6 items** — take `specFine` and the indev whenever those files are touched; `buf1` only if you want the 1.6 kB back.

All five code fixes together: < ~40 lines of firmware + a couple of strings, no protocol changes (F3's budget is
internal), no new state — safe to ship as one "memory hygiene" release or split per item.
