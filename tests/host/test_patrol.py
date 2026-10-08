"""Offline tests for C5 patrol on the host: the "pt" member in hello / the {"t":"pt"} status line / the "patrol" ack,
the countdown in /api/state, and the /api/cmd route (validation, 409 refusals, 503 without a device)."""
import shutil
import unittest
from unittest import mock

from _support import bh, make_bw, feed, FakeClock, HELLO_24
from test_http import Server

PT = {"leg": 1, "left": 12000, "cyc": 2, "legs": [["spec", 30], ["both", 40], ["ble", 20]]}


class PatrolStateTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()

    def test_old_firmware_without_pt_keeps_none(self):
        feed(self.bw, HELLO_24)   # no "pt" member at all
        self.assertIsNone(self.bw.snapshot()["patrol"])
        self.assertFalse(self.bw.patrolling())

    def test_hello_pt_null_is_idle(self):
        feed(self.bw, dict(HELLO_24, pt=None))
        self.assertEqual(self.bw.snapshot()["patrol"], {"on": False})
        self.assertFalse(self.bw.patrolling())

    def test_status_line_and_countdown(self):
        clock = FakeClock()
        with mock.patch.object(bh.time, "time", clock):
            feed(self.bw, dict(HELLO_24, pt=None))
            feed(self.bw, {"t": "pt", "pt": PT})
            p = self.bw.snapshot()["patrol"]
            self.assertTrue(p["on"])
            self.assertEqual((p["leg"], p["cyc"], p["band"], p["left_ms"]), (1, 2, "both", 12000))
            self.assertEqual(p["legs"], [["spec", 30], ["both", 40], ["ble", 20]])
            self.assertNotIn("t", p)
            clock.advance(1.5)
            self.assertEqual(self.bw.snapshot()["patrol"]["left_ms"], 10500)
            clock.advance(60)
            self.assertEqual(self.bw.snapshot()["patrol"]["left_ms"], 0)   # due: floors at 0 until the hand-off
        self.assertTrue(self.bw.patrolling())

    def test_hello_ack_and_stop(self):
        feed(self.bw, dict(HELLO_24, band="spec", pt=dict(PT, leg=0, left=30000)))
        self.assertEqual(self.bw.snapshot()["patrol"]["band"], "spec")
        feed(self.bw, {"t": "ack", "cmd": "patrol", "pt": dict(PT, leg=2)})
        self.assertEqual(self.bw.snapshot()["patrol"]["band"], "ble")
        feed(self.bw, {"t": "ack", "cmd": "patrol", "pt": None})
        self.assertEqual(self.bw.snapshot()["patrol"], {"on": False})
        feed(self.bw, {"t": "pt", "pt": PT})
        feed(self.bw, {"t": "pt", "pt": None})   # the line the device sends once when a patrol ends
        self.assertFalse(self.bw.patrolling())

    def test_other_acks_leave_patrol_alone(self):
        feed(self.bw, {"t": "pt", "pt": PT})
        feed(self.bw, {"t": "ack", "cmd": "band", "band": "2.4g"})
        self.assertTrue(self.bw.patrolling())

    def test_malformed_pt_does_not_escape(self):
        feed(self.bw, {"t": "pt", "pt": {"leg": "x", "legs": []}})   # logged as a bad line, state unchanged
        self.assertIsNone(self.bw.snapshot()["patrol"])
        self.assertTrue(any("bad 'pt' line" in l for l in self.bw.snapshot()["log"]))


class CleanLegsTest(unittest.TestCase):
    def test_good(self):
        for v, want in [
            ([["spec", 30], ["both", 40], ["ble", 20]], "spec:30,both:40,ble:20"),
            ([("2.4g", 5), ("154", 600)], "2.4g:5,154:600"),
            ([{"mode": "5g", "sec": 10}, {"mode": "spec", "sec": "15"}], "5g:10,spec:15"),
            ("both:5,ble:5", "both:5,ble:5"),
            ([["ble", 5]] * 6, ",".join(["ble:5"] * 6)),
        ]:
            with self.subTest(v=v):
                self.assertEqual(bh.clean_patrol_legs(v), want)

    def test_bad(self):
        for v in ([["spec", 30]], [["ble", 5]] * 7, [], [["6g", 30], ["ble", 5]], [["spec", 4], ["ble", 5]],
                  [["spec", 601], ["ble", 5]], [["spec", 30.5], ["ble", 5]], [["spec", True], ["ble", 5]],
                  [["spec", "30s"], ["ble", 5]], [["spec", None], ["ble", 5]], [["spec"], ["ble", 5]],
                  [["spec", 30, 1], ["ble", 5]], "spec:30", "spec:30,ble", "spec:30,ble:5\nreboot", "spec:30;ble:5",
                  [["spec reboot", 30], ["ble", 5]], [[None, 30], ["ble", 5]], 7, None, {"spec": 30}):
            with self.subTest(v=v):
                self.assertIsNone(bh.clean_patrol_legs(v))


class PatrolRouteTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        feed(self.bw, dict(HELLO_24, pt=None))
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)
        self.addCleanup(shutil.rmtree, self.bw.captures_dir, True)

    def sent_for(self, body):
        self.bw.ser.clear()
        code, resp = self.s.post(body)
        return code, self.bw.ser.lines()

    def test_good(self):
        for body, want in [
            ({"cmd": "patrol", "value": True}, ["patrol 1"]),
            ({"cmd": "patrol", "value": 1}, ["patrol 1"]),
            ({"cmd": "patrol", "value": "on"}, ["patrol 1"]),
            ({"cmd": "patrol", "value": False}, ["patrol 0"]),
            ({"cmd": "patrol", "value": 0}, ["patrol 0"]),
            ({"cmd": "patrol"}, ["patrol 0"]),
            ({"cmd": "patrol", "legs": [["both", 5], ["ble", 5]]}, ["patrol both:5,ble:5"]),
            ({"cmd": "patrol", "legs": "spec:30,both:40,ble:20"}, ["patrol spec:30,both:40,ble:20"]),
        ]:
            with self.subTest(body=body):
                self.assertEqual(self.sent_for(body), (200, want))

    def test_bad_input_is_400_and_sends_nothing(self):
        for body in ({"cmd": "patrol", "value": 2}, {"cmd": "patrol", "value": "maybe"}, {"cmd": "patrol", "value": []},
                     {"cmd": "patrol", "legs": [["spec", 30]]}, {"cmd": "patrol", "legs": [["x", 30], ["ble", 5]]},
                     {"cmd": "patrol", "legs": "spec:3,ble:5"}, {"cmd": "patrol", "legs": [["ble", 5]] * 7},
                     {"cmd": "patrol", "legs": "spec:30,ble:5\nreboot"}):
            with self.subTest(body=body):
                self.assertEqual(self.sent_for(body), (400, []))

    def test_not_connected_is_503(self):
        self.bw.ser = None
        for body in ({"cmd": "patrol", "value": True}, {"cmd": "patrol", "value": False},
                     {"cmd": "patrol", "legs": [["both", 5], ["ble", 5]]}):
            with self.subTest(body=body):
                self.assertEqual(self.s.post(body)[0], 503)

    def test_start_refused_while_capture_hunt_or_deauth(self):
        for setup, what in [({"cap": 1}, "capture"), ({"sd": {"cap": 1}}, "capture"),
                            ({"hunt": {"mac": "aa:bb:cc:dd:ee:ff"}}, "hunt"), ({"deauth": {"mac": "x"}}, "deauth")]:
            with self.subTest(what=setup):
                saved = {k: self.bw.state.get(k) for k in setup}
                self.bw.state.update(setup)
                try:
                    code, sent = self.sent_for({"cmd": "patrol", "value": True})
                    self.assertEqual((code, sent), (409, []))
                    self.assertEqual(self.sent_for({"cmd": "patrol", "value": False}), (200, ["patrol 0"]))   # stop always
                finally:
                    self.bw.state.update(saved)

    def test_starts_refused_while_patrolling_but_stops_pass(self):
        feed(self.bw, {"t": "pt", "pt": PT})
        for body in ({"cmd": "capture", "value": 1}, {"cmd": "sdcap", "value": 1},
                     {"cmd": "hunt", "mac": "aa:bb:cc:dd:ee:ff"}, {"cmd": "deauth", "mac": "11:22:33:44:55:66"},
                     {"cmd": "dca", "client_mac": "aa:aa:aa:aa:aa:aa", "ap_bssid": "11:22:33:44:55:66"},
                     {"cmd": "explain", "value": "full"}):
            with self.subTest(body=body):
                code, resp = self.s.post(body)
                self.assertEqual(code, 409)
                self.assertIn(b"stop patrol first", resp)
        self.assertIsNone(self.bw.snapshot()["capture"])   # no empty pcap was opened
        for body, want in [({"cmd": "sdcap", "value": 0}, ["sdcap 0"]), ({"cmd": "hunt"}, ["hunt 0"]),
                           ({"cmd": "deauth"}, ["deauth 0"]), ({"cmd": "dca"}, ["dca 0"]),
                           ({"cmd": "band", "value": "5g"}, ["band 5g"]),   # a manual band ends the patrol on the device
                           ({"cmd": "patrol", "value": False}, ["patrol 0"])]:
            with self.subTest(body=body):
                self.assertEqual(self.sent_for(body), (200, want))


if __name__ == "__main__":
    unittest.main()
