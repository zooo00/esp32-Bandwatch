"""Offline tests for the HTTP hardening (review S-H1..H4) and the host-side robustness fixes that came with it:
Host/Origin/Content-Type checks, security headers, body validation, 503 with no device, dest-only Wi-Fi rows
with null rssi/max, and the bounded buffers / race fixes in the Bandwatch object."""
import http.client
import json
import os
import shutil
import struct
import time
import unittest
from unittest import mock

from _support import bh, make_bw, feed, HELLO_24, wifi_row
from test_http import Server


def request(s, method, path, body=None, headers=None, host=None):
    """Raw http.client request so Host/Origin/Content-Type/Content-Length can be anything."""
    port = s.srv.server_address[1]
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    c.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
    c.putheader("Host", host if host is not None else "127.0.0.1:%d" % port)
    hdrs = dict(headers or {})
    if body is not None and "Content-Length" not in hdrs:
        hdrs["Content-Length"] = str(len(body))
    for k, v in hdrs.items():
        c.putheader(k, v)
    c.endheaders()
    if body:
        try:
            c.send(body)
        except OSError:
            pass   # the server may answer (and close) before reading an oversized body
    r = c.getresponse()
    data = r.read()
    out = (r.status, data, dict(r.headers))
    c.close()
    return out


JSON = {"Content-Type": "application/json"}


class HeadersAndHostTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        feed(self.bw, HELLO_24)
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)
        self.port = self.s.srv.server_address[1]

    def test_security_headers_on_every_response(self):
        for method, path, body in (("GET", "/", None), ("GET", "/api/state", None), ("GET", "/nope", None),
                                   ("GET", "/file?name=missing.pcap", None),
                                   ("POST", "/api/cmd", b'{"cmd":"info"}'), ("POST", "/api/cmd", b"{bad")):
            with self.subTest(method=method, path=path):
                _, _, h = request(self.s, method, path, body, JSON if body else None)
                self.assertEqual(h.get("X-Frame-Options"), "DENY")
                self.assertEqual(h.get("X-Content-Type-Options"), "nosniff")
                self.assertEqual(h.get("Content-Security-Policy"), "frame-ancestors 'none'")

    def test_host_header_allowlist(self):
        good = ["127.0.0.1:%d", "localhost:%d", "LOCALHOST:%d", "[::1]:%d"]
        bad = ["evil.example:%d", "127.0.0.1", "127.0.0.1:1", "rebind.attacker.test", "", "127.0.0.2:%d"]
        for h in good:
            with self.subTest(host=h):
                self.assertEqual(request(self.s, "GET", "/api/state", host=h % self.port if "%" in h else h)[0], 200)
        for h in bad:
            with self.subTest(host=h):
                host = h % self.port if "%" in h else h
                code, body, _ = request(self.s, "GET", "/api/state", host=host)
                self.assertEqual(code, 403)
                self.assertEqual(json.loads(body), {"ok": False, "error": "bad host"})
                self.bw.ser.clear()
                self.assertEqual(request(self.s, "POST", "/api/cmd", b'{"cmd":"info"}', JSON, host=host)[0], 403)
                self.assertEqual(self.bw.ser.lines(), [])

    def test_host_allowed_rules(self):
        self.assertTrue(bh.host_allowed("whatever.lan:8080", ["0.0.0.0"], 8080))   # wildcard: permissive
        self.assertTrue(bh.host_allowed("x", ["::"], 8080))
        self.assertTrue(bh.host_allowed("192.168.1.5:8080", ["192.168.1.5"], 8080))
        self.assertTrue(bh.host_allowed("mybox.local:8080", ["192.168.1.5", "mybox.local"], 8080))
        self.assertTrue(bh.host_allowed("[fe80::1]:8080", ["fe80::1"], 8080))
        self.assertTrue(bh.host_allowed("localhost", ["127.0.0.1"], 80))           # default port may be omitted
        self.assertFalse(bh.host_allowed("localhost", ["127.0.0.1"], 8080))
        self.assertFalse(bh.host_allowed(None, ["127.0.0.1"], 8080))
        self.assertFalse(bh.host_allowed("evil:8080", ["192.168.1.5"], 8080))

    def test_wildcard_bind_warns(self):
        class Stop(Exception):
            pass

        def fake_server(addr, handler):
            raise Stop

        for bind, warned in (("0.0.0.0", True), ("192.168.1.5", True), ("127.0.0.1", False), ("localhost", False)):
            with self.subTest(bind=bind), \
                    mock.patch("sys.argv", ["bandwatch_host.py", "--no-oui-download", "--port", "/dev/null-x",
                                            "--bind", bind]), \
                    mock.patch.object(bh, "ThreadingHTTPServer", lambda a, h: mock.Mock(
                        serve_forever=mock.Mock(side_effect=Stop))), \
                    mock.patch.object(bh.threading, "Thread"), \
                    mock.patch.object(bh.signal, "signal"), \
                    mock.patch.object(bh.OuiDb, "load_cache", return_value=True), \
                    mock.patch("builtins.print") as pr:
                with self.assertRaises(Stop):
                    bh.main()
                text = " ".join(str(a) for c in pr.call_args_list for a in c.args)
                self.assertEqual("NO authentication" in text, warned)


class PostValidationTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw()
        feed(self.bw, HELLO_24)
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)
        self.addCleanup(shutil.rmtree, self.bw.captures_dir, True)
        self.port = self.s.srv.server_address[1]

    def post(self, body, headers):
        self.bw.ser.clear()
        code, data, _ = request(self.s, "POST", "/api/cmd", body, headers)
        return code, json.loads(data) if data else None, self.bw.ser.lines()

    def test_content_type_required(self):
        for ct in (None, "text/plain", "application/x-www-form-urlencoded", "multipart/form-data; boundary=x",
                   "application/jsonp"):
            with self.subTest(ct=ct):
                code, resp, sent = self.post(b'{"cmd":"deauth","mac":"11:22:33:44:55:66"}',
                                             {"Content-Type": ct} if ct else {})
                self.assertEqual(code, 415)
                self.assertFalse(resp["ok"])
                self.assertEqual(sent, [])
        for ct in ("application/json", "application/json; charset=utf-8", "Application/JSON;charset=UTF-8"):
            with self.subTest(ct=ct):
                self.assertEqual(self.post(b'{"cmd":"info"}', {"Content-Type": ct})[:3:2], (200, ["info"]))

    def test_origin_must_match_host(self):
        for origin in ("http://evil.example", "http://127.0.0.1:1", "null", "file://", "http://localhost:%d" % self.port):
            with self.subTest(origin=origin):
                code, resp, sent = self.post(b'{"cmd":"info"}', dict(JSON, Origin=origin))
                self.assertEqual((code, resp["error"], sent), (403, "foreign origin", []))
        code, resp, sent = self.post(b'{"cmd":"info"}', dict(JSON, Origin="http://127.0.0.1:%d" % self.port))
        self.assertEqual((code, resp, sent), (200, {"ok": True}, ["info"]))

    def test_content_length_checked(self):
        for cl, want in (("abc", 400), ("-1", 400), ("1e3", 400), (str(bh.HTTP_BODY_MAX + 1), 413)):
            with self.subTest(cl=cl):
                code, resp, sent = self.post(b'{"cmd":"info"}', dict(JSON, **{"Content-Length": cl}))
                self.assertEqual((code, resp["ok"], sent), (want, False, []))

    def test_body_must_be_object(self):
        for body in (b"[]", b"5", b"null", b'"info"', b"[{\"cmd\":\"info\"}]", b"{bad"):
            with self.subTest(body=body):
                code, resp, sent = self.post(body, JSON)
                self.assertEqual((code, resp["ok"], sent), (400, False, []))

    def test_errors_are_json_with_ok_false(self):
        for body, code in ((b'{"cmd":"nope"}', 400), (b'{"cmd":"deauth","mac":"x"}', 400),
                           (b'{"cmd":"park","value":"six"}', 400)):
            with self.subTest(body=body):
                c, resp, _ = self.post(body, JSON)
                self.assertEqual(c, code)
                self.assertIs(resp["ok"], False)
                self.assertIsInstance(resp["error"], str)
        code, data, _ = request(self.s, "POST", "/api/other", b"{}", JSON)
        self.assertEqual((code, json.loads(data)["ok"]), (404, False))

    def test_snaplen_clamped(self):
        for snap, want in ((5000, "snap 1600"), (8, "snap 32"), ("256", "snap 256")):
            with self.subTest(snap=snap):
                code, _, sent = self.post(json.dumps({"cmd": "capture", "value": 1, "snaplen": snap}).encode(), JSON)
                self.assertEqual((code, sent), (200, [want, "cap 1"]))
                self.bw.stop_capture()

    def test_bad_snaplen_opens_no_file(self):
        for snap in ("big", "12x", [1]):
            with self.subTest(snap=snap):
                code, resp, sent = self.post(json.dumps({"cmd": "capture", "value": 1, "snaplen": snap}).encode(), JSON)
                self.assertEqual((code, sent), (400, []))
                self.assertIsNone(self.bw.snapshot()["capture"])
                self.assertEqual([f for f in os.listdir(self.bw.captures_dir) if f.endswith(".pcap")], [])


class NotConnectedTest(unittest.TestCase):
    def setUp(self):
        self.bw = make_bw(with_serial=False)
        self.s = Server(self.bw)
        self.addCleanup(self.s.close)
        self.addCleanup(shutil.rmtree, self.bw.captures_dir, True)

    def post(self, obj):
        code, data, _ = request(self.s, "POST", "/api/cmd", json.dumps(obj).encode(), JSON)
        return code, json.loads(data)

    def test_device_commands_503(self):
        for obj in ({"cmd": "info"}, {"cmd": "band", "value": "ble"}, {"cmd": "deauth", "mac": "11:22:33:44:55:66"},
                    {"cmd": "hunt", "mac": "aa:bb:cc:dd:ee:ff"}, {"cmd": "hunt"}, {"cmd": "sdcap", "value": 1},
                    {"cmd": "huntssid", "ssid": "HomeNet"}, {"cmd": "huntssid"},
                    {"cmd": "capture", "value": 1}, {"cmd": "capture", "value": 0},
                    {"cmd": "sdrm", "path": "events.csv"}, {"cmd": "sdread", "path": "events.csv"},
                    {"cmd": "explain", "value": "full"}):
            with self.subTest(obj=obj):
                self.assertEqual(self.post(obj), (503, {"ok": False, "error": "not connected"}))
        self.assertEqual(os.listdir(self.bw.captures_dir) if os.path.isdir(self.bw.captures_dir) else [], [])
        st = self.bw.snapshot()
        self.assertIsNone(st["capture"])
        self.assertIsNone(st["sd_rm"])          # no "pending" delete for a device that is not there
        self.assertIsNone(st["explain"])

    def test_validation_still_400_and_host_side_ok(self):
        self.assertEqual(self.post({"cmd": "band", "value": "6g"})[0], 400)
        self.assertEqual(self.post({"cmd": "huntssid", "ssid": "x" * 33})[0], 400)   # validated before the device
        self.assertEqual(self.post({"cmd": "explain", "value": "clear"}), (200, {"ok": True}))

    def test_capture_send_failure_removes_file(self):
        class DeadSerial:
            def write(self, data):
                raise OSError("gone")

        self.bw.ser = DeadSerial()
        self.assertEqual(self.post({"cmd": "capture", "value": 1})[0], 503)
        self.assertEqual([f for f in os.listdir(self.bw.captures_dir) if f.endswith(".pcap")], [])
        self.assertIsNone(self.bw.pcap)


class DestOnlyTest(unittest.TestCase):
    def test_dest_only_rssi_and_max_null(self):
        bw = make_bw()
        bw.merge_wifi([wifi_row("aa:bb:cc:00:00:01", rssi=0, mx=-40, ch=0, flags=4),
                       wifi_row("aa:bb:cc:00:00:02", rssi=-60, mx=-50, ch=6, flags=0)])
        st = json.loads(json.dumps(bw.snapshot()))
        devs = {d["mac"]: d for d in st["wifi_devs"]}
        d1, d2 = devs["aa:bb:cc:00:00:01"], devs["aa:bb:cc:00:00:02"]
        self.assertTrue(d1["dest_only"])
        self.assertIsNone(d1["rssi"])
        self.assertIsNone(d1["max"])
        self.assertEqual(d1["hist"], [])         # no fake 0 dBm sparkline point
        self.assertEqual((d2["rssi"], d2["max"], len(d2["hist"])), (-60, -50, 1))

    def test_dest_only_on_a_channel_does_not_break_spec_spans(self):
        bw = make_bw()
        bw.merge_wifi([wifi_row("aa:bb:cc:00:00:01", rssi=0, mx=0, ch=6, flags=4)])
        spans = bw._spec_known_spans(time.time())
        self.assertEqual([s["str"] for s in spans], [-128])


class RobustnessTest(unittest.TestCase):
    def test_line_splitter_bounds_buffer(self):
        dropped = []
        ls = bh.LineSplitter(limit=100, on_overflow=lambda: dropped.append(1))
        self.assertEqual(ls.feed(b"a\r\nb"), [b"a"])
        self.assertEqual(ls.feed(b"x" * 150), [])
        self.assertEqual((len(ls.buf), len(dropped)), (0, 1))
        self.assertEqual(ls.feed(b"y" * 150), [])            # still the same line: one notice, buffer bounded
        self.assertEqual((len(ls.buf), len(dropped)), (0, 1))
        self.assertEqual(ls.feed(b"tail\nnext\n"), [b"next"])  # the dropped line's tail is not a line of its own

    def test_device_tables_capped(self):
        bw = make_bw()
        with mock.patch.object(bh, "DEV_MAX", 5):
            for i in range(8):
                bw.merge_wifi([wifi_row("02:00:00:00:00:%02x" % i, age=1000 * (8 - i))])
        self.assertEqual(len(bw.wifi_devs), 5)
        self.assertNotIn("02:00:00:00:00:00", bw.wifi_devs)   # the stalest went first
        self.assertIn("02:00:00:00:00:07", bw.wifi_devs)

    def test_probe_ssids_capped(self):
        bw = make_bw()
        for i in range(bh.PROBE_SSIDS_MAX + 10):
            feed(bw, {"t": "pr", "mac": "aa:bb:cc:dd:ee:ff", "ssid": "net%d" % i, "rssi": -50, "ch": 6})
        self.assertEqual(len(bw.probes["aa:bb:cc:dd:ee:ff"]["ssids"]), bh.PROBE_SSIDS_MAX)

    def test_spec_hist_cleared_on_step_change(self):
        bw = make_bw()
        feed(bw, {"t": "fs", "step": 2, "lo": 2400, "count": 2, "bins": [[-90, -85, -80, 5], [-90, -85, -70, 5]]})
        self.assertEqual(set(bw.spec_hist), {2400, 2402})
        feed(bw, {"t": "fs", "step": 5, "lo": 2400, "count": 2, "bins": [[-90, -85, -80, 5], [-90, -85, -70, 5]]})
        self.assertEqual(set(bw.spec_hist), {2400, 2405})
        feed(bw, {"t": "fd", "mhz": 2401, "step": 1, "min": -90, "mean": -85, "max": -60, "ns": 4})
        self.assertEqual(bw.fine["bins"], [])
        self.assertEqual(bw.spec_hist, {})

    def test_explain_clears_active_when_planning_throws(self):
        bw = make_bw()
        with mock.patch.object(bw, "_explain_legs_current", side_effect=RuntimeError("boom")):
            with self.assertRaises(RuntimeError):
                bw._explain_run("current")
        self.assertFalse(bw.state["explain"]["active"])

    def test_same_second_captures_get_unique_names(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        self.addCleanup(shutil.rmtree, bw.captures_dir, True)
        with mock.patch.object(bh.time, "strftime", return_value="bandwatch-wifi-20261007-101010"):
            paths = []
            for _ in range(3):
                paths.append(bw.start_capture()["file"])
                bw.stop_capture()
        self.assertEqual(len(set(paths)), 3)
        self.assertTrue(paths[1].endswith("-2.pcap"))

    def test_stop_capture_twice_counts_once(self):
        bw = make_bw()
        feed(bw, HELLO_24)
        self.addCleanup(shutil.rmtree, bw.captures_dir, True)
        bw.start_capture()
        bw.stop_capture()
        bw.stop_capture()
        self.assertEqual(bw.state["saved"]["usb"]["count"], 1)

    def test_pcap_orig_len_never_below_incl_len(self):
        d = make_bw().captures_dir
        self.addCleanup(shutil.rmtree, d, True)
        w = bh.PcapWriter(os.path.join(d, "t.pcap"))
        w.write(6, -50, 0, 2, b"abcdef")   # device claims a shorter original than it sent
        w.close()
        w.write(6, -50, 0, 6, b"abcdef")   # after close: ignored, no exception
        with open(os.path.join(d, "t.pcap"), "rb") as f:
            raw = f.read()
        incl, orig = struct.unpack("<II", raw[24 + 8:24 + 16])
        self.assertEqual(incl, orig)

    def test_sdread_short_file_not_saved(self):
        import base64
        bw = make_bw()
        self.addCleanup(shutil.rmtree, bw.captures_dir, True)
        name = "bandwatch-wifi-20261007-101010.pcap"
        feed(bw, {"t": "ack", "cmd": "sdread", "file": "/" + name, "bytes": 6})
        feed(bw, b"S 3 " + base64.b64encode(b"abc"))   # second chunk lost
        feed(bw, {"t": "ack", "cmd": "sdread_done", "sent": 6})
        st = bw.snapshot()["sd_read"]
        self.assertEqual((st["done"], st["failed"], st["received"]), (False, True, 3))
        self.assertFalse(os.path.exists(os.path.join(bw.captures_dir, name)))
        # a chunk whose byte count disagrees with its payload also fails the pull
        feed(bw, {"t": "ack", "cmd": "sdread", "file": "/" + name, "bytes": 3})
        feed(bw, b"S 2 " + base64.b64encode(b"abc"))
        feed(bw, {"t": "ack", "cmd": "sdread_done", "sent": 3})
        self.assertTrue(bw.snapshot()["sd_read"]["failed"])
        self.assertFalse(os.path.exists(os.path.join(bw.captures_dir, name)))

    def test_file_download_name_rules(self):
        bw = make_bw()
        self.addCleanup(shutil.rmtree, bw.captures_dir, True)
        s = Server(bw)
        self.addCleanup(s.close)
        for n in ("ok.pcap", 'a"b.pcap', "notes.txt"):
            with open(os.path.join(bw.captures_dir, n), "wb") as f:
                f.write(b"data")
        code, body, h = s.get("/file?name=ok.pcap")
        self.assertEqual((code, body, h["Content-Disposition"]), (200, b"data", 'attachment; filename="ok.pcap"'))
        self.assertEqual(s.get("/file?name=a%22b.pcap")[0], 404)   # would break out of the quoted filename
        self.assertEqual(s.get("/file?name=notes.txt")[0], 404)    # only pcaps and csvs are served
        self.assertEqual(s.get("/file?name=ok.pcap%0d%0aX-Evil:%201")[0], 404)


if __name__ == "__main__":
    unittest.main()
