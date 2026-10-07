# Bandwatch tests

Three tiers. All use only the Python standard library (`unittest`) plus pyserial; nothing needs pytest.
Run everything from the repository root.

| Tier | Command | Needs | Time |
| --- | --- | --- | --- |
| 1. host | `python3 -m unittest discover -s tests/host` | Python 3 + pyserial (imported by the host tool) | ~6 s |
| 2. firmware | `python3 -m unittest discover -s tests/firmware` | `arduino-cli` + the ESP32 core (`./setup.sh`) | seconds (cached) to minutes (clean) |
| 3. device | `BANDWATCH_PORT=/dev/cu.usbmodemXXXX python3 -m unittest discover -s tests/device -v` | a running board on USB | ~6-7 min |

`tests/run_offline.sh` runs tiers 1 and 2 (extra arguments, e.g. `-v`, go to unittest).

## 1. Host (`tests/host/`) - offline, runs anywhere

Builds the real `Bandwatch` host object with no serial port (the constructor never opens one; a `FakeSerial`
records what `send()` would write), feeds it protocol lines through `handle_line()`, and asserts on
`snapshot()`.

- `test_protocol.py`: `hello` (channels, hunt, deauth, sd, ev, band change closing a host pcap), `d` / `s` dwell
  and sweep lines (C6 top talker kept across sweeps), `fs` / `fd` fine spectrum, chunked `w` lines merging by MAC
  (v1.17), IE decoding, tier-1 `dest_only` rows and surveillance tiers, `_resolve_parents` (join, late AP,
  ambiguous suffix, self-reference, sticky suffix), BLE `b` rows + `ble` heartbeat, 802.15.4 `z` rows, C1 `pr`
  probes (grouping by SSID, randomized-MAC counting, 15-minute expiry on a faked clock, `seeking` on device rows,
  table cap), C4 `ev` in hello / ack / `{"t":"ev"}`, the `sdcap ... recording stopped` error clearing `sd.cap`,
  `sd card removed|inserted` logs driving `sd.mounted`, acks.
- `test_sdread_and_robustness.py`: `sdread` chunk reassembly into the captures dir, the short-read error path
  (no file written), orphan / bad chunks, a device-supplied name that tries to leave the captures dir; ~60
  malformed or truncated lines that must not raise out of `handle_line()` or break `snapshot()`, and an end-to-end
  `reader()` session (fake port, lines split across reads) that must survive all of them; `send()` newline
  collapsing.
- `test_mirror.py`: LCD-mirror `M` / `MF` framing - regions published only at markers, legacy firmware without
  markers published per region, fallback when markers stop, torn (`complete=0`) frames held up to
  `MIRROR_HOLD_S`, stale back buffer published by the status poll, MF sequence gaps and 16-bit wrap, malformed
  regions, and the `screen_status()` `complete` flag (regression test, see below).
- `test_http.py`: the real `make_handler()` on an ephemeral port - the dashboard at `/` and `/v2`, `/classic`
  a 301 to `/` (classic removed in v1.19), `--ui` accepted and ignored, a missing page 404s, `/api/state`, `/api/screen`, `/screen.bin`, `/file`
  (download + traversal), and every `/api/cmd` mapped to its exact device string; bad input (bad MACs, newline
  injection, unknown values, `sdread` names outside `CARD_TEXT_FILES` / the pcap pattern, path traversal, > 39
  chars) answered 400 with nothing sent.
- `test_security.py` (v1.19.3): the HTTP hardening - anti-framing headers on every response, the `Host` allowlist
  (and the wildcard-bind warning), `POST /api/cmd` Content-Type / Origin / Content-Length (400/413/415) / non-object
  bodies, the `{"ok":false,"error"}` error shape, 503 when not connected with no empty pcap, `dest_only` rows with
  `rssi`/`max` null, and the robustness fixes (bounded line splitter, capped device tables and probe SSIDs,
  `spec_hist` cleared on a step change, `explain` always clearing `active`, unique same-second capture names, pcap
  `orig_len`, a short `sdread` not saved, `/file` name rules).
- `test_sdrm.py`: deleting a card file - `card_file_name()` (the guard `sdread` and `sdrm` share: pcaps and
  `CARD_TEXT_FILES` incl. `seen.old.csv`, 39-char edge; traversal, nested paths, `//`, 40 chars, other files,
  non-strings rejected), `POST sdrm` mapping and 400s, the `sdrm` ack (ok drops the file from the cached listing,
  lowers `file_total`, re-sends `sdls`; ok 0 keeps it), `sdrm: ...` refusal err lines, the pulled local copy left
  alone, and a failed `sdread` reporting `failed` so the dashboard stops showing progress.

## 2. Firmware (`tests/firmware/`) - offline, no board

- `test_build_static_ram` runs `./build.sh` (compile only, never `--upload`) and fails if `Global variables use N
  bytes` exceeds `global_bytes_ceiling` in `static_ram_ceiling.json` (ceiling 77,696 B = 77,184 B measured at v1.19.3 + 512 B;
  restamp the JSON's `measured_*` fields at the next release that moves static RAM). Raise the ceiling deliberately, in the same commit as the
  change that needs it.
  `BANDWATCH_BUILD_LOG=<file>` parses an existing build log instead of compiling.
- The `static_assert`s in `bandwatch/devices.h` (`WifiDev` 64 B, `BleDev` 48 B, `Dev154` 24 B) and the channel
  table assert in `bandwatch_core.h` are covered by the compile itself: if one fires, `./build.sh` fails and so
  does this test.
- `test_lcd_strings_ascii` scans every string literal in `bandwatch/lcd_ui.cpp` (comments ignored) for
  non-ASCII characters or `\x`/`\u`/octal escapes above 0x7E: Montserrat lacks glyphs such as U+00B7 and draws
  them as boxes.

## 3. Device (`tests/device/`) - hardware, run by hand

**Stop `host/bandwatch_host.py` first** - it holds the port. The suite opens the port exclusively with DTR and RTS
asserted and HUPCL cleared, so neither opening nor closing makes an edge (CLAUDE.md rule 1); it never reboots,
flashes, or runs esptool. Env: `BANDWATCH_PORT` (required; without it the tier skips), `BANDWATCH_SOAK_S`
(per-mode soak, default 30).

Covers: `info` / hello fields and version format; `band both|ble|154|spec|5g|2.4g` acks, hello `chs` and
mode traffic; unknown command / band; `park` in Wi-Fi (dwells pinned, resume after `park 0`, refused channels)
and in spec (the `fd` MHz stays on the parked channel's centre bin, `fs` pauses); `cap 1` / `cap 0` ring log, `P`
lines, refusal in spec; `cap 1` + `sdcap 1` while walking every LCD page keeps free heap >= 24 kB (skipped
without a card); `events 1` ack shape and `{"t":"ev"}`; `sdprobe` R1 in {1, 255}; `mirror 1` `M` + `MF` lines,
silence after `mirror 0`; `page next` cycle and `page prev`; a soak per mode (plus spec with the mirror on) where
every line must be well-formed JSON or a valid `P`/`S`/`M`/`MF` line; `w` chunks of at most 24 rows; `sdrm` (bad
names refused before the card is touched; with a card: a short `sdcap` pcap found by `sdls`, deleted, gone from
the next `sdls`, a second delete answers `no such file`; deleting the file being recorded is refused and the
recording keeps running); and no reboot during the run.

Every test restores what it changed (band, park, USB/SD capture, mirror, event log) against the hello read at the
start. Side effects that remain: a band change also selects an LCD page, and the `sdcap` heap test leaves one small
pcap on the card (the `sdrm` tests delete their own).

## Quick check after a flash: `tools/smoke.py`

Not a test tier, a ~15 s sanity pass: `python3 tools/smoke.py [port]` (host tool stopped; port defaults to the
first `/dev/cu.usbmodem*`). Read-only - it sends only `info`, `sdinfo` and `sdls`, so nothing is transmitted or
persisted. Checks the hello arrives (retried ~30 s after a flash), its version matches `kVersion` in
`bandwatch_core.h` (`--any-version` skips that), free heap is above the 24 kB floor, a status line shows the radio
running, `sdinfo`/`sdls` answer, and every JSON line parses. Exit 0 only if all pass.

## Host fixes made alongside these tests

- `screen_status()` returned `"complete": <bool>` merged with `**screen_stats`, whose complete-frame counter
  was also named `complete` and overwrote the flag (the v2 dashboard showed "in sync" for any frame once one
  complete frame had arrived). The counter is now `complete_frames`.
- `handle_line()`: valid JSON that is not an object (`42`, `[1]`, `null` - e.g. boot noise) raised out of the
  error handler (`msg.get`) and ended the serial session.
- `merge_wifi/merge_ble/merge_154`: a malformed row inserted a half-built device without `last`, after which
  every later merge and every `snapshot()` (so `/api/state`) raised `KeyError` until restart. Rows are now
  inserted only after they parse. A `hello` whose `chs` is not a list is rejected before it changes state.
- `POST sdread`: the pcap-name pattern (`\S+`) accepted `/` and `\`, e.g. `bandwatch-wifi-../../x.pcap`; the
  name may no longer contain path separators.
