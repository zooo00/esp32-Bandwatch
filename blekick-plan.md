# Plan: `blekick` — the deauth equivalent for BLE

Development brief, self-contained. Repo state when written: **main @ 66e759b (v1.20.1)**. Read
`CLAUDE.md`, `AGENTS.md` and `docs/DEVELOPER.md` §13 (BLE capture) first — this brief assumes you'll follow
their conventions; anchors below are function names + line numbers at that commit.

## Mission

One new serial command, **`blekick <mac>`**, makes Bandwatch a NimBLE *central* for one target MAC in BLE
mode: it repeatedly **connects → holds ~600 ms → disconnects with status 0x13** ("Remote User Terminated
Connection"). For a single-connection-slot peripheral (a beach speaker playing from its phone) that evicts or
stutters the real peer. Loops until stopped by `blekick 0` or the dead-man's switch — exactly how Wi-Fi
`deauth <bssid>` behaves, and deliberately mirrors it in everything else (stop on mode change, not persisted,
red LED blink while active).

The beach test: speaker in range → `band ble` → Kick on its row → music stops/stutters, `kicks` climbs.
Stop → music resumes. A speaker that refuses new peers while paired shows `kicks=0, fails climbing` — that is
the *honest* "busy" signal (see Risks), not a bug to hide.

## Why connection takeover (prior art)

There is no BLE deauth *management frame*; the approaches split into four families:

| Family | Mechanism | Prior art | Us? |
| --- | --- | --- | --- |
| Advertising flood | crafted ADV spam, no pairing | [Flipper Zero / DEFCON iPhone crashes](https://en.wikipedia.org/wiki/Bluetooth_Low_Energy_denial_of_service_attacks) | later — hits the phone's popup path, not a speaker's music link |
| **Connection takeover** | connect into the victim's slot and leave; repeat | [blue-deauth](https://github.com/its0x08/blue-deauth)'s "connect flood" | **this plan (v1)** |
| L2CAP data flood | hold a connection, hammer echo/data packets | l2ping / Bluedoser / BDS — all of blue-deauth's type 1; [GHM DoS paper](https://dergipark.org.tr/en/download/article-file/1495282) (DOI 10.18466/cbayarfbe.856119): single l2ping did nothing, ~7 *parallel* ones disconnected headphones; already-paired speakers rejected entry | v2 candidate if takeover doesn't bite ("flood" mode) |
| Raw LL terminate (passive) | follow the victim's live connection and inject a forged `LL_TERMINATE_IND` (0x13); no pairing, no connect needed | [btlejack "jam"](https://github.com/virtualabs/btlejack): an nRF51 coprocessor first recovers AA / CRCInit / channel map; its active "hijack" variant is admitted time-sensitive against latency-0 BLE5 devices | v3 candidate: works even on busy-refuse victims, but needs raw PDU injection — a second board (we have the `tools/witness` S3 pattern) |

Connection takeover is the only family doable with stock NimBLE host APIs on this core (no raw PDU injection —
that's Ubertooth-class hardware — and no GATT/L2CAP client). ([DE-auth of the Blue](https://sprout.ics.uci.edu/pubs/deauth_blue.pdf)
is a different meaning of "deauth" entirely — RSSI-shadowing presence detection; naming inspiration only.) Even bettercap has not
shipped one: its [#719](https://github.com/bettercap/bettercap/issues/719) ("Will we ever have a BLE DoS attack?!"), filed in 2020 by
blue-deauth's own author (its0x08), is still open with no comments.

## Scope (in / out)

**In:** `ble_kick.cpp` (new), its struct/tunables/decls in `bandwatch_core.h`, the command branch + heartbeat
member in `host_proto.cpp`, one stop line in `setBandMode()` (`bandwatch.cpp`), two patrol-guard one-liners,
one LED line in `lcd_ui.cpp`; host-py plumbing; **the Kick button on BLE table rows** (plus its highlight and
click latch); version bump + short docs + tests.

**Out — do NOT touch these** (they were considered and cut):
- `"bk"` member in `hello` / the hello serial budget (`sendHello`) — the 1 Hz BLE heartbeat self-heals state within ≤1 s.
- A dedicated action bar (`#kickBar`, a third sibling of `#deauthBar`/`#huntBar`). Live counters surface via log lines instead.
- The system-page "… - kick \<mac\>" LCD line in `refreshSystem()`.
- Alert-blip dropping during an attack in `led_alert.cpp` (2 spots).
- NVS persistence: like park/hunt/deauth, a reboot stops transmitting — no new setting key.
- Capture paths, `blescan` policy, the discovery callback itself (`bleGapEvent` keeps handling DISC/DISC_COMPLETE only; the kick registers its own GAP connect callback).

## Command + protocol (exact shapes)

```
blekick aa:bb:cc:dd:ee:ff     start (BLE mode + parseable MAC)
blekick 0                     stop
                              unparseable non-empty arg OR not in BLE mode = stop (mirror `deauth` exactly)
```

- ack:    `{"t":"ack","cmd":"blekick","bk":["aa:bb:cc:dd:ee:ff","idle",0,0]}`  or `"bk":null` when stopped
          — array is `[mac, state, kicks, fails]`, state ∈ `"idle" | "connecting" | "connected"`
- heartbeat (BLE mode only): `sendBleStatus()` gains the same `"bk"` member before its closing brace;
          raise its budget check 300 → ~360 B. (§14 rule: BLE mode has no dwell lines, so any live counter
          the host needs must ride this line.)
- log lines (the on-hardware oracle): `{"t":"log","msg":"blekick #N connected"}` on every successful connect;
  `{"t":"log","msg":"blekick attempt failed rc=<nimble code>"}` on refused attempts, first one + every 16th.
  Wording flexible, keep the greppable prefix. Every send through the usual `serialRoom()`/`sendLinef` discipline (rule 6).

## Firmware spec

### `bandwatch_core.h`
Tunables next to the BLE ones (~line 38):
```cpp
constexpr uint32_t kBleKickHoldMs = 600;      // hold a won connection this long before terminating it
constexpr uint32_t kBleKickGapMs = 700;       // pause between attempts (let the victim re-advertise)
constexpr int      kBleKickTimeoutMs = 4000;  // per-attempt connect timeout for ble_gap_connect()
```
Struct next to `BleState` (~line 268); field order load-bearing as always (target ≈ 28 B, add a `static_assert`):
```cpp
// BLE kick ("blekick"): the deauth equivalent for BLE — we become a central for one target and loop
// connect / hold / terminate(0x13). Not persisted: a reboot must stop transmitting.
struct BleKick {
    uint8_t mac[6] = {};                 // target, MSB-first (table order)
    volatile bool active = false;        // attack running ("blekick <mac>" was accepted in BLE mode)
    volatile uint8_t st = 0;             // 0 idle / 1 connecting / 2 connected
    uint8_t addrType = 1;                // BLE_ADDR_PUBLIC/RANDOM, resolved from the device table at start (default random)
    uint16_t connh = 0;                  // connection handle while st==2
    uint32_t startMs = 0;                // dead-man's switch: kDeauthMaxMs, like deauth
    uint32_t stateMs = 0;                // pacing: millis() when st last changed (and at start)
    volatile uint32_t kicks = 0;         // successful connects — the moment of impact
    volatile uint32_t fails = 0;         // refused/timed-out attempts
};
```
Instance lives in `ble_kick.cpp` (`BleKick bleKick;`) and is externed here next to `extern BleState bleScan;`,
plus decls `void startBleKick(const uint8_t* mac); void stopBleKick(); void serviceBleKick();`.

### `bandwatch/ble_kick.cpp` (new, ~150 lines with comments)
Model the file on `deauth_diag.cpp`'s shape and comment density. Includes: `"bandwatch_core.h"`,
`<BLEDevice.h>` + `<host/ble_gap.h>` (what `ble_scan.cpp` uses; add `<host/ble_hs.h>`/`<nimble/hci_common.h>`
only if `ble_addr_t`/`BLE_OWN_ADDR_PUBLIC` don't resolve).

NimBLE facts for this core (Arduino-ESP32 3.3.11, verified in the toolchain headers):
```c
int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr, int32_t duration_ms,
                    const struct ble_gap_conn_params *params,   // NULL = stack defaults (fine)
                    ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);  // BLE_HS_ENOTCONN if already gone
```
- `ble_addr_t { uint8_t type; uint8_t val[6]; }` — **`.val` is little-endian** (on-air order): reverse the
  table MAC when filling it. `BLE_OWN_ADDR_PUBLIC = 0`, random = 1.
- 0x13 "Remote User Terminated Connection": this port's enum calls it `BLE_ERR_REM_USER_CONN_TERM`
  (`include/nimble/ble.h`) — use a named local constant, e.g.
  `constexpr uint8_t kKickReason = 0x13;`, with the spec citation in the comment (btlejack's jam injects the same
  reason code passively — prior art for the value, not the mechanism).

**startBleKick(mac)** (loop task): under `g_devMux` copy the MAC, resolve `addrType` by scanning
`bleDevs[]` for a fresh entry (`d.lastMs && macEq(...)`) — default `BLE_ADDR_RANDOM` if evicted/never seen;
reset counters and `startMs = stateMs = millis()`, `st = 0`; set `active = true` last.

**serviceBleKick()** (called from `uiTimerCb()` in `lcd_ui.cpp`, one line after `serviceDeauth();` — ~120 ms
cadence, plenty for 600/700 ms pacing):
- `!active`: if `st == 2` call `ble_gap_terminate(bk.connh, kKickReason)` once (a stop mid-connected must not
  hold the victim's slot until its supervision timeout), then return.
- dead-man: `millis() - startMs > kDeauthMaxMs` → log "blekick auto-stopped after 5 min" + `stopBleKick()` (copy deauth's).
- `st == 0` and `now - stateMs >= kBleKickGapMs`: build the peer `ble_addr_t`, call `ble_gap_connect(
  BLE_OWN_ADDR_PUBLIC, &peer, kBleKickTimeoutMs, nullptr, bleKickEventCb, nullptr)`. rc==0 → under lock
  `st = 1, stateMs = now`. rc!=0 → fail path (below).
- `st == 2` and `now - stateMs >= kBleKickHoldMs`: `ble_gap_terminate(bk.connh, kKickReason)`; on non-zero
  return (ENOTCONN — peer dropped first) set `st = 0, stateMs = now` under lock (the DISCONNECT event will find
  the state already idle and skip).

**bleKickEventCb(struct ble_gap_event*, void*)** (NimBLE host task; same single-producer discipline as
`bleGapEvent()`): mutate shared fields only under `g_devMux`, keep heap/Serial out of the lock, print after:
- `BLE_GAP_EVENT_CONNECT`, rc==0: `kicks++, connh = event->connect.conn_id, st = 2, stateMs = now`; log
  "blekick #N connected".
- `BLE_GAP_EVENT_CONNECT`, rc!=0 (refused/timeout): if `st == 1` → `fails++, st = 0, stateMs = now`; throttled log.
- `BLE_GAP_EVENT_DISCONNECT`: act only when `event->disconnect.conn_id == connh` AND `st == 2` → `st = 0,
  stateMs = now`. (A kick counts from its *connect*; the disconnect reason is diagnostic — fold it into a log if cheap.)
- everything else: ignore.

**stopBleKick()** (loop task): `if (!active) return;` under lock `active = false, st = 0`. (The pending
CONNECT request either fires its callback harmlessly against the state guards above, or dies with the host
when a mode change deinits it — see next.)

### `host_proto.cpp`
- Command branch after the `"dca"` one (~line 765): parse with the existing `parseMac()` (colons and dashes).
  Start iff `bandMode == BAND_BLE && parseMac(...)`, else stop. Ack per protocol above (through `sendLinef`).
- `printBleKick()` helper beside `printDeauth()` (~line 190) writing `"bk":null` or the 4-element array; call it
  in `sendBleStatus()` before its final `"}\n"` and raise that function's budget 300 → ~360.
- **Patrol guards** (C5 v1 refuses to run over parked/transmitting activities — kick behaves like deauth):
  - the mid-patrol guard list in `handleCommand()` (~line 584): add `"blekick"` next to hunt/huntssid/deauth/dca.
  - `startPatrol` refusals (~line 610): after the `deauth.active` check, `else if (!why && bleKick.active) why = "stop kick first";`.

### `bandwatch.cpp` — `setBandMode()` (line ~234)
One line after `stopDeauth();`: `stopBleKick();   // a mode change deinits the host: don't strand a pending connect`.
This covers manual band changes *and* patrol hand-offs (both route through `setBandMode`).

### `lcd_ui.cpp` — `driveLed()` (line ~1302)
One line: `if (deauth.active || bleKick.active) { ... }` — the red attack blink. (Order already puts it before
`ledBlipActive()`, so blips yield to a kick too, same as deauth.)

## Host (`host/bandwatch_host.py`)

- state init (~line 509): `"blekick": None,   # {"mac","state","kicks","fails"} while the kick loop runs`
- `_set_bkick(bk)` beside `_set_deauth()` (~line 720): list of ≥4 → dict; `null`/empty → None.
- dispatch: in the `t == "ble"` branch (heartbeat) and in the `ack` branch for `cmd == "blekick"`, call it on
  `msg.get("bk")`.
- `_snapshot_locked()`: add `"blekick"` to the deep-copied keys tuple (~line +33 of that function, next to `"deauth","ble"`).
- `_command()` (~line 1978): 
  ```python
  elif cmd == "blekick":
      mac = clean_mac(req.get("mac"))
      if req.get("mac") and not mac: raise _Reply(400, "bad mac")
      dev(f"blekick {mac or '0'}")
  ```
  plus patrol parity: add `"blekick"` to the `not_while_patrolling` list (~line 1935) and a `else "blekick" if st.get("blekick") else None`
  term in the busy chain (~line 1961).

## Dashboard (`host/dashboard2.html`) — the button, only the button

- `renderBle()` (line 1643): row template ends `...${huntBtn(d.mac, 0, s)}</td>` (line 1655) → append
  `${kickBtn(d, s)}`. Helper next to `huntBtn` (~line 1294 area in the current file):
  ```js
  function kickBtn(d, s) {
    const on = !!(s.blekick && s.blekick.mac === d.mac);
    return pending.bk ? `<button class="small" disabled>…</button>`
      : `<button class="small ${on ? 'stop' : ''}" data-bkick="${on ? '' : d.mac}" title="Connect, hold, disconnect — repeat (the BLE deauth)">${on ? 'Stop' : 'Kick'}</button>`;
  }
  ```
- click handler beside the `data-hunt`/`data-deauth` ones (~line 630s): read `k.dataset.bkick`,
  `armPending('bk', k, '…')`, `post({ cmd: 'blekick', mac: <mac or null> })` (null stops — same shape as deauth).
- latch: add `bk: 0` to the `pending` object (line 534); release on every render like deauth does in
  `renderDeauthBar()` (~line 1239): after ~6 s or when `!!s.blekick` differs from the previous render's value
  (`prev.bkOn`).
- highlight: line 1656 row class — add `${s.blekick && s.blekick.mac === d.mac ? 'kicked' : ''}` (the CSS class exists, line 134).

## Version + docs

- `bandwatch_core.h`: `kVersion` **1.20.1 → 1.21**.
- `CHANGELOG.md`: one entry (mirror the v1.20.x style).
- `CLAUDE.md`: command list in "Testing without LCD" — "`blekick <mac>` | `blekick 0` (BLE mode only; connect/hold/disconnect loop, the deauth equivalent for BLE)"; append a line to the version history at the bottom.
- `docs/DEVELOPER.md`: short **§23** (~30 lines): mechanism + why takeover (cite blue-deauth, btlejack's jam — same 0x13 reason code — bettercap #719, the GHM paper DOI 10.18466/cbayarfbe.856119,
   and the Wikipedia BLE-DoS article), state machine, tunables, protocol members, RAM cost, open questions (busy-refuse → v2 flood; resolvable-private addresses show as fails).

## Tests checklist

1. **Host tier** (`tests/host/`): `test_protocol.py` — a `"ble"` line carrying `"bk":[mac,state,kicks,fails]`
   lands in `snapshot()["blekick"]`; null clears it; the `ack cmd=blekick` path does too. `test_http.py` —
   POST `/api/cmd {"cmd":"blekick","mac":"aa:bb:…"}` → exact serial string `blekick aa:bb:…`; no/`"0"` mac →
   `blekick 0`; bad MAC present → 400; (optionally) patrol-conflict 409 mirroring the deauth cases.
2. **Firmware tier** (`tests/firmware/`): build and read "Global variables use N bytes"; expect ~+30–60 B over
   current `measured_bytes` 77184; update `static_ram_ceiling.json` (bump ceiling, note why — its own convention).
3. **Device tier** (`tests/device/test_device.py`): small round trip in BLE mode: `set_band("ble")`, send
   `blekick <any valid MAC>` → ack with non-null `bk`; a `"ble"` heartbeat within ~1.5 s carries `bk`; then
   `blekick 0` → ack null. (No need to assert kicks — that needs real air.)

## Risks / open questions

1. **Busy victim refuses entry** while paired (the GHM paper's speakers): `kicks=0, fails ↑`. v2 = parallel
   attempts or an L2CAP echo flood ("`blekick` --flood"?) — the paper's "7 threads" insight. The passive raw-terminate
   family (prior-art table) defeats busy-refuse outright at the cost of a coprocessor board. v1 reports honestly.
2. **Resolvable-private addresses** fail to connect (no IRKs held) → also honest `fails`; static random + public work.
3. Stop mid-connect may still win one connection; covered by the `!active && st==2` terminate in `serviceBleKick()`.
4. NimBLE default connection params are fine for close range; no TX-power fiddling (unlike Wi-Fi deauth's 16 dBm raise).

## Project rules that bite here (from AGENTS.md / CLAUDE.md)

- Run `tests/run_offline.sh` before committing; report the static RAM delta in the commit/session summary.
- Don't flash or poke serial unless told (DTR/RTS asserted, rule 1); the owner owns the board.
- Every serial line checks room first and drops whole (rule 6) — use `serialRoom()` / `sendLinef()`.
- Concurrency: gap calls from the loop task; NimBLE callbacks on the host task; shared fields under `g_devMux`
  with heap/Serial kept out of the lock (§10).
- Commit with your own identity + trailing co-author line per AGENTS.md.
