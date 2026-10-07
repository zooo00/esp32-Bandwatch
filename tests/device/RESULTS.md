# Tier-3 (on-board) results

One row per release run, so the next run has something to compare against. Run with the host tool stopped:
`BANDWATCH_PORT=/dev/cu.usbmodemXXXX python3 -m unittest discover -s tests/device -v`.

| Date | Firmware | Result | Time | Notes |
| --- | --- | --- | --- | --- |
| 2026-10-07 | v1.19 | **26/26 OK** | 328 s | Card present (no skips), event log armed, no reboot during the run (T99). B4 recheck passed — both fresh opens after the smoke pass got hello JSON in <0.1 s; host tool had been holding the port over the pause (killed it, board did not reset). |
| 2026-10-07 | v1.19 | **26/26 OK** | 332 s | Card present, event log armed. New: `T11SdRm` (deleted a real pcap, refused the file being recorded), `T12Alerts`. First attempt failed in `setUpModule` only because it ran seconds after a flash; the first hello now retries for ~30 s. |
| 2026-10-07 | v1.18.2 | **19/19 OK** | 308 s | First run of the suite. `T08Page.test_page_prev_is_inverse_of_next` caught the `page prev` bug (fixed in this release). |

## Related on-board measurements

| Date | Firmware | Measurement | Value |
| --- | --- | --- | --- |
| 2026-10-07 | v1.19 | Min free heap: `events 1` + `cap 1` + `sdcap 1`, `both`, every LCD page | **25,172 B** (Overview); floor 24,576 B |
| 2026-10-07 | v1.18.1 | Mirror frames complete (20 s, Wi-Fi pages / BLE Devices) | 57% / 0% (BLE list exceeds the link) |
| 2026-10-07 | v1.18 | SD presence probe, empty slot / card | R1 0xFF / 0x01; removal logged ~1 s after the first absent reading |
| 2026-10-07 | v1.15.5 | Free heap per page, `both`, idle | Overview 98.9 kB, Devices 106.4, Channels 106.4, System 102.3 |
