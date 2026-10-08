"""Hardware tier: protocol tests against a connected, running Bandwatch board.

    BANDWATCH_PORT=/dev/cu.usbmodem101 python3 -m unittest discover -s tests/device -v

Stop the host tool first (it holds the port; the port is opened exclusively, so a clash fails at once instead
of interleaving two readers). The port is opened with DTR/RTS asserted and HUPCL cleared - no edges, no reset
(CLAUDE.md rule 1). Nothing here reboots the board, flashes it, or runs esptool.

Each test restores what it changed (band, park, USB capture, SD capture, mirror, event log - compared against
the hello read at the start); the LCD page is restored by the page test, but a band change also picks a page,
so the page shown after a run can differ. `sdcap` (only when a card is in) leaves one small pcap on the card
(the T11 sdrm tests delete the ones they record).

Env: BANDWATCH_PORT (required), BANDWATCH_SOAK_S (per-mode soak length, default 30).
Runtime: ~7-8 minutes with the default soak.
"""
import os
import re
import time
import unittest

from board import (Board, validate_line, BAND_NAMES, EXPECTED_CHS, EV_KEYS, LCD_W, LCD_H, MIN_FREE_HEAP,
                   WIFI_ROWS_PER_LINE, M_RE, MF_RE, P_RE, MAC_RE)

PORT = os.environ.get("BANDWATCH_PORT")
SOAK_S = float(os.environ.get("BANDWATCH_SOAK_S", "30"))

board = None
ORIG = None          # the hello read before any test ran


def setUpModule():
    global board, ORIG
    if not PORT:
        raise unittest.SkipTest("set BANDWATCH_PORT to the board's serial port to run the hardware tier")
    board = Board(PORT)
    time.sleep(0.5)
    # Right after a flash or reboot the board needs several seconds (boot photo, mode card, radio start, the event
    # log loading its card baseline) before it answers "info": retry for up to ~30 s instead of failing the module.
    for attempt in range(4):
        try:
            ORIG = board.hello(timeout=8)
            break
        except AssertionError:
            if attempt == 3:
                raise
            time.sleep(1)


def tearDownModule():
    if board is not None:
        try:
            restore()
        finally:
            board.close()


def restore():
    """Put band / park / capture / SD capture / mirror / event log / LED alerts back to what ORIG reported."""
    h = board.hello(timeout=8)
    if h.get("pt"):   # C5: never leave the board walking (a patrol would also change the band under the checks below)
        board.command("patrol 0")
    if h.get("mir") != ORIG.get("mir"):
        board.command("mirror %d" % (ORIG.get("mir") or 0))
    if (h.get("sd") or {}).get("cap") and not (ORIG.get("sd") or {}).get("cap"):
        board.command("sdcap 0")
    if h.get("cap") and not ORIG.get("cap"):
        board.command("cap 0")
    if h.get("band") != ORIG["band"]:
        board.set_band(ORIG["band"], settle=1.0)
    if (board.hello().get("park") or 0) != (ORIG.get("park") or 0):
        board.command("park %d" % (ORIG.get("park") or 0))
    if ORIG.get("cap") and not h.get("cap"):
        board.command("cap 1")
    ev0, ev1 = ORIG.get("ev"), h.get("ev")
    if ev0 is not None and ev1 is not None and ev0.get("on") != ev1.get("on"):
        board.command("events %d" % ev0.get("on"))
    if "alerts" in ORIG and h.get("alerts") != ORIG["alerts"]:
        board.command("alerts %d" % ORIG["alerts"])
    board.clear()


class BoardTest(unittest.TestCase):
    def setUp(self):
        board.clear()

    def tearDown(self):
        restore()

    def assertAllValid(self, lines, context):
        bad = [(why, raw[:120]) for raw in lines for why in [validate_line(raw)] if why]
        self.assertEqual(bad[:10], [], "%d malformed line(s) %s (first 10 shown)" % (len(bad), context))


class T01Hello(BoardTest):
    def test_info_hello_fields(self):
        h = board.hello()
        self.assertEqual(h["fw"], "bandwatch")
        self.assertRegex(h["ver"], r"^\d+\.\d+(\.\d+)?$")
        for k in ("dwell_ms", "spec_step", "band", "country", "bandmode", "proto", "promisc", "chs", "park", "cap",
                  "snap", "heap", "up", "rst", "hunt", "h", "deauth", "sd", "mir", "ev", "alerts", "pt"):
            self.assertIn(k, h)
        self.assertIn(h["band"], BAND_NAMES)
        self.assertEqual(h["chs"], EXPECTED_CHS[h["band"]])
        self.assertIn(h["spec_step"], (1, 2, 5))
        self.assertGreater(h["dwell_ms"], 0)
        self.assertGreater(h["heap"], MIN_FREE_HEAP)
        self.assertIn(h["cap"], (0, 1))
        self.assertIn(h["mir"], (0, 1))
        self.assertIn(h["alerts"], (0, 1))
        self.assertTrue(32 <= h["snap"] <= 1600)
        self.assertIsInstance(h["rst"], str)
        self.assertEqual(set(h["sd"]), {"mounted", "mb", "cap", "file", "frames", "bytes", "err", "clock"})
        self.assertEqual(set(h["ev"]), EV_KEYS)
        self.assertTrue(all(isinstance(v, int) for v in h["ev"].values()))

    def test_unknown_command_is_an_error(self):
        idx = board.mark()
        board.send("frobnicate")
        board.wait_json(lambda o: o.get("t") == "err" and o.get("msg") == "unknown command", 3, idx)


class T02Bands(BoardTest):
    def check_mode_traffic(self, mode, chs):
        idx = board.mark()
        if mode == "ble":
            o = board.wait_json(lambda o: o.get("t") == "ble", 6, idx, "a BLE heartbeat")
            self.assertIn("devs", o)
        elif mode == "spec":
            o = board.wait_json(lambda o: o.get("t") == "fd", 6, idx, "an fd line")
            self.assertTrue(2400 <= o["mhz"] <= 2483)
        else:
            o = board.wait_json(lambda o: o.get("t") == "d", 6, idx, "a dwell line")
            self.assertIn(o["c"], chs)

    def test_band_round_trips(self):
        for mode in ("both", "ble", "154", "spec", "5g", "2.4g"):
            with self.subTest(mode=mode):
                if board.band == mode:
                    board.set_band("ble" if mode != "ble" else "154")
                idx = board.mark()
                board.send("band " + mode)
                ack = board.wait_json(lambda o: o.get("t") == "ack" and o.get("cmd") == "band", 15, idx, "band ack")
                self.assertEqual(ack["band"], mode)
                h = board.wait_json(lambda o: o.get("t") == "hello", 10, idx, "hello after band")
                self.assertEqual(h["band"], mode)
                self.assertEqual(h["chs"], EXPECTED_CHS[mode])
                time.sleep(1.5)
                self.check_mode_traffic(mode, EXPECTED_CHS[mode])

    def test_unknown_band_keeps_mode(self):
        before = board.hello()["band"]
        ack = board.command("band 6g", timeout=8)
        self.assertEqual(ack["band"], before)


class T03Park(BoardTest):
    def test_park_wifi(self):
        board.set_band("2.4g")
        self.assertEqual(board.command("park 6")["park"], 6)
        time.sleep(1.0)                                    # let the in-flight dwell finish
        idx = board.mark()
        time.sleep(3.0)
        ds = board.json_since(idx, "d")
        self.assertGreaterEqual(len(ds), 2)
        self.assertEqual({d["c"] for d in ds}, {6})
        self.assertEqual({d["park"] for d in ds}, {6})
        self.assertEqual(board.command("park 0")["park"], 0)
        idx = board.mark()
        time.sleep(4.0)
        self.assertGreater(len({d["c"] for d in board.json_since(idx, "d")}), 1, "still parked after park 0")

    def test_park_channel_not_in_mode(self):
        board.set_band("2.4g")
        self.assertEqual(board.command("park 36")["park"], 0)
        self.assertEqual(board.command("park 999")["park"], 0)

    def test_park_spec_holds_fine_sweep(self):
        board.set_band("spec")
        step = board.hello()["spec_step"]
        self.assertEqual(board.command("park 15")["park"], 15)
        # bandwatch.cpp advanceChannel(): the bin nearest the 15.4 channel's centre (2425 MHz)
        centre = 2405 + 5 * (15 - 11)
        want = 2400 + ((centre - 2400 + step // 2) // step) * step
        time.sleep(1.0)
        idx = board.mark()
        time.sleep(4.0)
        fds = board.json_since(idx, "fd")
        self.assertGreaterEqual(len(fds), 3)
        self.assertEqual({o["mhz"] for o in fds}, {want}, "parked spec sweep must hold one bin")
        self.assertEqual(board.json_since(idx, "fs"), [], "full sweeps (fs) must pause while parked")
        self.assertEqual(board.command("park 0")["park"], 0)
        idx = board.mark()
        time.sleep(3.0)
        self.assertGreater(len({o["mhz"] for o in board.json_since(idx, "fd")}), 1, "sweep did not resume")


class T04Capture(BoardTest):
    def test_cap_ring_and_frames(self):
        board.set_band("154")                              # a mode change releases any old ring ...
        board.set_band("2.4g")                             # ... so cap 1 has to allocate a fresh one
        idx = board.mark()
        ack = board.command("cap 1")
        self.assertEqual(ack["cap"], 1, "cap 1 refused: %s" % board.json_since(idx, "err"))
        logs = [o["msg"] for o in board.json_since(idx, "log")]
        ring = [m for m in logs if re.match(r"capture ring(: \d+ slots| re-sized to \d+ slots)", m)]
        self.assertTrue(ring, "no 'capture ring: N slots' log (got %s)" % logs)
        self.assertGreater(int(re.search(r"(\d+) slots", ring[0]).group(1)), 0)
        time.sleep(4.0)
        lines = board.raw_since(idx)
        ds = board.json_since(idx, "d")
        self.assertTrue(ds and all(d["cap"] == 1 for d in ds[1:]))
        p = [l for l in lines if l.startswith(b"P ")]
        if any(d["f"] for d in ds):
            self.assertTrue(p, "frames were counted on air but no P line arrived")
        self.assertAllValid(p, "in the P stream")
        self.assertEqual(board.command("cap 0")["cap"], 0)
        time.sleep(1.0)
        late = [l for l in board.collect(2.0) if l.startswith(b"P ")]
        self.assertEqual(late, [], "P lines kept coming after cap 0")

    def test_cap_refused_in_spec(self):
        board.set_band("spec")
        self.assertEqual(board.command("cap 1")["cap"], 0)

    def test_cap_and_sdcap_keep_heap_floor(self):
        if not card_present():
            self.skipTest("no microSD card in the slot")
        board.set_band("2.4g")
        self.assertEqual(board.command("cap 1")["cap"], 1)
        r = board.command_or_err("sdcap 1", timeout=10)
        self.assertEqual(r.get("t"), "ack", "sdcap 1 failed: %s" % r.get("msg"))
        self.assertEqual(r["sdcap"], 1)
        heaps = []
        idx = board.mark()
        # Walk every LCD page while both sinks run: the ring must leave room for the heaviest one (section 16).
        walk_pages(dwell=1.5)
        time.sleep(4.0)
        heaps += [o["heap"] for o in board.json_since(idx, "s") if o.get("heap")]
        heaps.append(board.hello()["heap"])
        self.assertGreaterEqual(min(heaps), MIN_FREE_HEAP,
                                "free heap fell to %d B with USB + SD capture (floor %d)" % (min(heaps), MIN_FREE_HEAP))
        ds = board.json_since(idx, "d")
        self.assertTrue(any(d.get("sdc") == 1 for d in ds))
        self.assertEqual(board.command("sdcap 0", timeout=10)["sdcap"], 0)
        self.assertEqual(board.command("cap 0")["cap"], 0)


def card_present():
    h = board.hello()
    if (h.get("sd") or {}).get("mounted"):
        return True
    for _ in range(3):
        r = board.command("sdprobe")
        if r["r1"] != -1:
            return r["r1"] == 1
        time.sleep(1.0)
    return False


class T05Events(BoardTest):
    def test_events_ack_shape(self):
        idx = board.mark()
        r = board.command_or_err("events 1", timeout=8)
        self.assertEqual(r.get("t"), "ack", "events 1 failed: %s" % r.get("msg"))
        self.assertEqual(set(r["ev"]), EV_KEYS)
        self.assertTrue(all(isinstance(v, int) for v in r["ev"].values()))
        self.assertEqual(r["ev"]["on"], 1)
        st = board.wait_json(lambda o: o.get("t") == "ev", 8, idx, "an ev status line while armed")
        self.assertEqual(set(st["ev"]), EV_KEYS)
        self.assertEqual(board.hello()["ev"]["on"], 1)
        if not ORIG["ev"]["on"]:
            r = board.command("events 0", timeout=8)
            self.assertEqual(r["ev"]["on"], 0)


class T06SdProbe(BoardTest):
    def test_sdprobe_r1(self):
        for _ in range(5):
            r = board.command("sdprobe")
            if r["r1"] != -1:                              # -1: card mounted/busy, the probe was not sent
                break
            time.sleep(1.0)
        self.assertIn(r["r1"], (1, 255), "CMD0 R1 should be 0x01 (card idle) or 0xFF (empty slot)")
        self.assertIn(r["present"], (0, 1))


class T07Mirror(BoardTest):
    def test_mirror_regions_and_markers(self):
        ack = board.command("mirror 1")
        self.assertEqual((ack["mirror"], ack["w"], ack["h"]), (1, LCD_W, LCD_H))
        idx = board.mark()
        end = time.monotonic() + 10
        while time.monotonic() < end:
            lines = board.raw_since(idx)
            if any(l.startswith(b"M ") for l in lines) and any(l.startswith(b"MF ") for l in lines):
                break
            time.sleep(0.25)
        lines = board.raw_since(idx)
        m = [l for l in lines if l.startswith(b"M ")]
        mf = [l for l in lines if l.startswith(b"MF ")]
        self.assertTrue(m, "no M region lines after mirror 1")
        self.assertTrue(mf, "no MF frame markers after mirror 1")
        self.assertAllValid(m + mf, "in the mirror stream")
        seqs = [int(MF_RE.match(l).group(1)) for l in mf]
        self.assertTrue(all(((b - a) & 0xFFFF) >= 1 for a, b in zip(seqs, seqs[1:])), "MF seq went backwards")
        self.assertEqual(board.command("mirror 0")["mirror"], 0)
        time.sleep(1.5)
        late = [l for l in board.collect(2.0) if l.startswith((b"M ", b"MF "))]
        self.assertEqual(late, [], "mirror lines kept coming after mirror 0")


def walk_pages(dwell=0.0):
    """Step the LCD once round every page available in this mode with `page next` (pausing `dwell` s on each)
    and end on the page it started from. Uses only `next`: see test_page_prev_is_inverse_of_next for why."""
    cycle = [board.command("page next")["page"]]
    for _ in range(7):
        time.sleep(dwell)
        p = board.command("page next")["page"]
        if p == cycle[0]:
            break
        cycle.append(p)
    else:
        raise AssertionError("page next never came back round: %s" % cycle)
    for _ in cycle[1:]:                                    # from cycle[0] to cycle[-1], the starting page
        board.command("page next")
    return cycle


class T08Page(BoardTest):
    def test_page_next_cycles_and_restores(self):
        cycle = walk_pages()
        self.assertTrue(all(0 <= p < 6 for p in cycle), cycle)
        self.assertEqual(len(set(cycle)), len(cycle), cycle)
        self.assertIn(5, cycle, "the System page is always available")

    def test_page_prev_is_inverse_of_next(self):
        """`page prev` should step back to the previous *available* page. lcd_ui.cpp stepPage() calls
        showPage(currentPage - 1), and showPage() skips unavailable pages by scanning *forward*, so when the page
        before the current one is unavailable (Spectrum outside spec mode, Hunt with no hunt) prev lands on the
        current page again and the LCD does not move."""
        cycle = walk_pages()                               # leaves the LCD where it was
        if len(cycle) < 2:
            self.skipTest("only one page available in this mode")
        start = cycle[-1]
        results = []
        for _ in cycle:                                    # try prev from every page in the cycle
            here = board.command("page next")["page"]
            back = board.command("page prev")["page"]
            want = cycle[(cycle.index(here) - 1) % len(cycle)]
            results.append((here, back, want))
            p = back
            while p != here:                               # re-sync onto `here` with next only
                p = board.command("page next")["page"]
        while board.command("page next")["page"] != start:   # back to the starting page
            pass
        wrong = [(h, b, w) for h, b, w in results if b != w]
        self.assertEqual(wrong, [], "page prev went (from, to, expected): %s; cycle %s" % (wrong, cycle))


class T09Soak(BoardTest):
    """No truncated or malformed line in any mode (rule 6: lines are dropped whole, never cut)."""

    def soak(self, mode, seconds, mirror=False):
        board.set_band(mode)
        if mirror:
            board.command("mirror 1")
        board.clear()
        board.send("info")
        time.sleep(seconds / 2)
        board.send("info")
        time.sleep(seconds / 2)
        lines = board.raw_since(0)
        board.clear()
        if mirror:
            board.command("mirror 0")
        return lines

    def test_every_line_parses_in_each_mode(self):
        for mode in ("5g", "2.4g", "both", "ble", "154", "spec"):
            with self.subTest(mode=mode):
                lines = self.soak(mode, SOAK_S)
                self.assertGreater(len(lines), 10, "almost nothing received in %s" % mode)
                self.assertAllValid(lines, "during a %.0f s soak in %s" % (SOAK_S, mode))
                ts = {l[6:9] for l in lines if l.startswith(b'{"t":"')}
                self.assertIn(b'hel', ts)

    def test_soak_with_mirror_in_spec(self):
        """The heaviest serial load: the mirror on the fast-changing spectrum page."""
        lines = self.soak("spec", max(10.0, SOAK_S / 2), mirror=True)
        self.assertTrue(any(l.startswith(b"M ") for l in lines))
        self.assertAllValid(lines, "with the mirror on in spec")


class T10WifiChunks(BoardTest):
    def test_w_chunks_at_most_24_rows(self):
        board.set_band("2.4g")
        idx = board.mark()
        board.send("info")
        time.sleep(12.0)
        ws = board.json_since(idx, "w")
        self.assertTrue(ws, "no w lines in 12 s")
        for w in ws:
            self.assertLessEqual(len(w["dev"]), WIFI_ROWS_PER_LINE)
            macs = [r[0] for r in w["dev"]]
            self.assertEqual(len(macs), len(set(macs)), "a MAC repeated inside one w line")
            for r in w["dev"]:
                self.assertEqual(len(r), 17)
                self.assertRegex(r[0], MAC_RE)
                self.assertRegex(r[16], r"^([0-9a-f]{6})?$")
        print("\n  w lines: %d, max rows/line %d, distinct MACs %d"
              % (len(ws), max(len(w["dev"]) for w in ws), len({r[0] for w in ws for r in w["dev"]})))


class T11SdRm(BoardTest):
    """sdrm (docs/DEVELOPER.md section 12): record a short pcap, find it with sdls, delete it, confirm it is gone."""

    def sdls(self):
        idx = board.mark()
        board.send("sdls")
        o = board.wait_json(lambda o: o.get("t") == "sdls" or (o.get("t") == "err" and str(o.get("msg", "")).startswith("sdls")),
                            10, idx, "an sdls listing")
        self.assertEqual(o.get("t"), "sdls", "sdls failed: %s" % o.get("msg"))
        return {str(n).lstrip("/"): sz for n, sz in o["files"]}, o

    def record_short(self, seconds=3.0):
        board.set_band("2.4g")
        r = board.command_or_err("sdcap 1", timeout=10)
        self.assertEqual(r.get("t"), "ack", "sdcap 1 failed: %s" % r.get("msg"))
        self.assertEqual(r["sdcap"], 1)
        self.assertTrue(r["file"].startswith("/bandwatch-wifi-"), r)
        time.sleep(seconds)
        return r["file"]

    def test_bad_names_refused_without_touching_the_card(self):
        for arg in ("../seen.csv", "/a/b.pcap", "/" + "x" * 40, "", "seen.csv\"x"):
            with self.subTest(arg=arg):
                r = board.command_or_err("sdrm " + arg, timeout=5)
                self.assertEqual(r.get("t"), "err", r)
                self.assertIn("bad file name", r["msg"])

    def test_sdrm_deletes_a_fresh_capture(self):
        if not card_present():
            self.skipTest("no microSD card in the slot")
        path = self.record_short()
        self.assertEqual(board.command("sdcap 0", timeout=10)["sdcap"], 0)
        name = path.lstrip("/")
        files, o = self.sdls()
        if name not in files and o["sent"] < o["total"]:
            self.skipTest("card listing truncated (%d of %d sent); cannot see the new file" % (o["sent"], o["total"]))
        self.assertIn(name, files)
        self.assertGreater(files[name], 24, "the pcap should hold more than its global header")
        r = board.command_or_err("sdrm " + path, timeout=10)
        self.assertEqual(r.get("t"), "ack", "sdrm failed: %s" % r.get("msg"))
        self.assertEqual((r["file"], r["ok"]), (path, 1))
        files, _ = self.sdls()
        self.assertNotIn(name, files)
        r = board.command("sdrm " + path, timeout=10)          # a second delete finds nothing
        self.assertEqual((r["ok"], r.get("msg")), (0, "no such file"))

    def test_sdrm_refused_while_recording(self):
        if not card_present():
            self.skipTest("no microSD card in the slot")
        path = self.record_short(seconds=1.5)
        try:
            r = board.command_or_err("sdrm " + path, timeout=10)
            self.assertEqual(r.get("t"), "err", "deleting the file being recorded must be refused: %s" % r)
            self.assertIn("being recorded", r["msg"])
            self.assertEqual(board.hello()["sd"]["cap"], 1, "the refusal must leave the recording running")
        finally:
            board.command("sdcap 0", timeout=10)
        r = board.command_or_err("sdrm " + path, timeout=10)   # stopped: now it may go (and leaves no litter)
        self.assertEqual((r.get("t"), r.get("ok")), ("ack", 1), r)


class T12Alerts(BoardTest):
    """LED alert blips (v1.19). The LED itself cannot be observed from here: these check the protocol and that a
    blip was started; watch the board for the colours (orange double / purple double / white single)."""

    def test_alerts_round_trip_and_persisted_in_hello(self):
        orig = board.hello()["alerts"]
        for want in (1 - orig, orig):
            r = board.command("alerts %d" % want)
            self.assertEqual(r["alerts"], want)
            self.assertEqual(board.hello()["alerts"], want)

    def test_ledtest_acks_each_kind(self):
        deauthing = bool(board.hello().get("deauth"))
        for kind in ("surv", "join", "new"):
            with self.subTest(kind=kind):
                r = board.command("ledtest %s" % kind)
                self.assertEqual(r["kind"], kind)
                self.assertIn(r["shown"], (0, 1))
                if not deauthing:   # a running deauth keeps the LED, so the blip is refused (shown 0)
                    self.assertEqual(r["shown"], 1)
                time.sleep(0.7)     # let the 300-500 ms pattern finish before the next

    def test_ledtest_works_with_alerts_off(self):
        orig = board.hello()["alerts"]
        board.command("alerts 0")
        try:
            r = board.command("ledtest surv")
            self.assertEqual(r["kind"], "surv")
        finally:
            board.command("alerts %d" % orig)

    def test_ledtest_bad_kind_is_an_error(self):
        idx = board.mark()
        board.send("ledtest purple")
        board.wait_json(lambda o: o.get("t") == "err" and str(o.get("msg", "")).startswith("ledtest"), 3, idx)


class T13Patrol(BoardTest):
    """C5 patrol (v1.20): short custom legs, the hand-off seen in hello and the {"t":"pt"} status line (also in BLE,
    which has no dwells), the refusals both ways, a manual band ending it, then "patrol 0". Only receive-side
    commands are tried while patrolling: a broken guard must not be able to start a deauth from a test."""

    def tearDown(self):
        board.send("patrol 0")
        board.send("hunt 0")
        time.sleep(0.3)
        super().tearDown()

    def test_bad_legs_refused(self):
        for arg in ("spec:30", "spec:4,ble:5", "spec:601,ble:5", "6g:30,ble:5", "spec:30,ble:5,"):
            with self.subTest(arg=arg):
                r = board.command_or_err("patrol " + arg)
                self.assertEqual(r.get("t"), "err", r)
        self.assertIsNone(board.hello()["pt"])

    def test_overlong_line_refused(self):
        idx = board.mark()
        board.send("patrol " + ",".join(["both:600"] * 7))   # 69 chars: past the 63-char line buffer
        board.wait_json(lambda o: o.get("t") == "err" and o.get("msg") == "line too long", 3, idx)
        self.assertIsNone(board.hello()["pt"])

    def test_patrol_refused_while_capturing(self):
        board.set_band("2.4g")
        if board.command("cap 1", timeout=8).get("cap") != 1:
            self.skipTest("no capture ring (low heap)")
        r = board.command_or_err("patrol 1")
        self.assertEqual((r.get("t"), r.get("msg")), ("err", "patrol: stop capture first"))
        board.command("cap 0")

    def test_hand_off_status_and_stop(self):
        ack = board.command("patrol both:5,ble:5", timeout=15)
        pt = ack["pt"]
        self.assertEqual(pt["legs"], [["both", 5], ["ble", 5]])
        self.assertEqual(pt["leg"], 0)
        self.assertTrue(0 < pt["left"] <= 5000, pt)
        idx = board.mark()
        h = board.wait_json(lambda o: o.get("t") == "hello" and (o.get("pt") or {}).get("leg") == 1, 12, idx,
                            "the hello of the second leg")
        self.assertEqual((h["band"], h["chs"]), ("ble", []))
        o = board.wait_json(lambda o: o.get("t") == "pt" and o.get("pt"), 4, idx, "a pt status line in BLE")
        self.assertIn(o["pt"]["leg"], (0, 1))
        h = board.wait_json(lambda o: o.get("t") == "hello" and (o.get("pt") or {}).get("leg") == 0, 12, idx,
                            "the hello of the wrap back to the first leg")
        self.assertEqual((h["band"], h["pt"]["cyc"]), ("both", 1))
        for cmd in ("cap 1", "sdcap 1", "hunt aa:bb:cc:dd:ee:ff"):
            with self.subTest(cmd=cmd):
                r = board.command_or_err(cmd)
                self.assertEqual((r.get("t"), r.get("msg")), ("err", cmd.split()[0] + ": stop patrol first"))
        self.assertIsNone(board.command("patrol 0")["pt"])
        h = board.hello()
        self.assertIsNone(h["pt"])
        mode = h["band"]
        time.sleep(6)   # longer than a leg: it really stopped
        self.assertEqual(board.hello()["band"], mode)

    def test_band_command_ends_patrol(self):
        board.command("patrol both:5,ble:5", timeout=15)
        idx = board.mark()
        board.send("band 2.4g")
        board.wait_json(lambda o: o.get("t") == "ack" and o.get("cmd") == "band", 15, idx, "band ack")
        # A leg hand-off may slip its own hello in before the band command lands: wait for the one with "pt" null.
        h = board.wait_json(lambda o: o.get("t") == "hello" and "pt" in o and o["pt"] is None, 10, idx,
                            "hello after band")
        self.assertEqual(h["band"], "2.4g")


class T99NoReboot(BoardTest):
    def test_board_did_not_reboot(self):
        h = board.hello()
        self.assertGreaterEqual(h["up"], ORIG["up"], "uptime went backwards: the board rebooted during the run "
                                                     "(rst=%s)" % h.get("rst"))


if __name__ == "__main__":
    unittest.main()
