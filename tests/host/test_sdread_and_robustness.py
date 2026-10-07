"""Offline tests: sdread chunk reassembly, its error path, and the rule that a malformed line never kills
the reader (docs/DEVELOPER.md section 4)."""
import base64
import os
import tempfile
import unittest
from unittest import mock

from _support import bh, make_bw, feed, FakeSerial, HELLO_24

PCAP = "/bandwatch-wifi-20261007-101010.pcap"


def s_line(data):
    return b"S %d " % len(data) + base64.b64encode(data)


class SdReadTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bw-sdread-")
        self.bw = make_bw(self.dir)

    def test_reassembles_and_writes_file(self):
        bw = self.bw
        payload = bytes(range(256)) * 3
        feed(bw, {"t": "ack", "cmd": "sdread", "file": PCAP, "bytes": len(payload)})
        st = bw.snapshot()["sd_read"]
        self.assertEqual((st["name"], st["total"], st["done"]), (os.path.basename(PCAP), len(payload), False))
        for i in range(0, len(payload), 100):
            feed(bw, s_line(payload[i:i + 100]))
        self.assertEqual(bw.snapshot()["sd_read"]["received"], len(payload))
        feed(bw, {"t": "ack", "cmd": "sdread_done", "sent": len(payload)})
        st = bw.snapshot()["sd_read"]
        self.assertTrue(st["done"])
        self.assertEqual(st["path"], os.path.join(self.dir, os.path.basename(PCAP)))
        with open(st["path"], "rb") as f:
            self.assertEqual(f.read(), payload)

    def test_card_text_file(self):
        bw = self.bw
        feed(bw, {"t": "ack", "cmd": "sdread", "file": "/events.csv", "bytes": 6})
        feed(bw, s_line(b"a,b,c\n"))
        feed(bw, {"t": "ack", "cmd": "sdread_done", "sent": 6})
        with open(os.path.join(self.dir, "events.csv"), "rb") as f:
            self.assertEqual(f.read(), b"a,b,c\n")

    def test_short_read_error_discards_partial_file(self):
        bw = self.bw
        feed(bw, {"t": "ack", "cmd": "sdread", "file": PCAP, "bytes": 10})
        feed(bw, s_line(b"12345"))
        feed(bw, {"t": "err", "msg": "sdread: read failed at 5 of 10 bytes (card removed?)"})
        st = bw.snapshot()["sd_read"]
        self.assertFalse(st["done"])
        self.assertEqual(st["received"], 5)
        self.assertNotIn("path", st)
        self.assertEqual(os.listdir(self.dir), [])
        feed(bw, s_line(b"67890"))                 # a straggler chunk has nowhere to land
        self.assertEqual(bw.snapshot()["sd_read"]["received"], 5)

    def test_no_card_error_without_start(self):
        feed(self.bw, {"t": "err", "msg": "sdread: no card"})
        self.assertIsNone(self.bw.snapshot()["sd_read"])

    def test_chunks_without_start_and_bad_chunks_ignored(self):
        bw = self.bw
        feed(bw, s_line(b"orphan"))
        self.assertIsNone(bw.snapshot()["sd_read"])
        feed(bw, {"t": "ack", "cmd": "sdread", "file": PCAP, "bytes": 3})
        feed(bw, b"S 3 ***")                       # not base64
        feed(bw, b"S")                             # truncated
        feed(bw, s_line(b"abc"))
        feed(bw, {"t": "ack", "cmd": "sdread_done", "sent": 3})
        with open(os.path.join(self.dir, os.path.basename(PCAP)), "rb") as f:
            self.assertEqual(f.read(), b"abc")

    def test_device_supplied_name_cannot_escape_captures_dir(self):
        bw = self.bw
        feed(bw, {"t": "ack", "cmd": "sdread", "file": "/../../evil.pcap", "bytes": 1})
        feed(bw, s_line(b"x"))
        feed(bw, {"t": "ack", "cmd": "sdread_done", "sent": 1})
        self.assertEqual(os.listdir(self.dir), ["evil.pcap"])


GARBAGE = [
    b"", b"   ", b"{", b"}", b"\xff\xfe\x00garbage", b"ESP-ROM:esp32c5-eco2-20250121", b"rst:0x1 (POWERON),boot:0x1c",
    b"42", b"null", b"true", b'"just a string"', b"[1,2,3]", b"[]", b"{}", b'{"t":null}', b'{"t":42}',
    b'{"t":"hello","fw":"bandwatch","ver":"1.18","chs":[1,2',                 # truncated mid-line
    b'{"t":"hello","chs":"notalist"}', b'{"t":"hello","chs":null,"h":[1]}',
    b'{"t":"d"}', b'{"t":"d","c":6}', b'{"t":"d","c":6,"s":1,"r":1,"f":1,"b":1,"st":1,"u":1,"g":1,"n":1,"park":0,'
                                    b'"cap":0,"drop":0,"h":[1,2]}',
    b'{"t":"s"}', b'{"t":"s","n":1,"g":1,"ch":[[1,2]]}', b'{"t":"s","n":1,"g":1,"ch":null}',
    b'{"t":"w"}', b'{"t":"w","dev":"x"}', b'{"t":"w","dev":[[1,2],null,"x",{}]}',
    b'{"t":"w","dev":[["aa:bb:cc:dd:ee:ff",-50,-45,10,500,6,null,""]]}',
    b'{"t":"b","dev":[["zz:zz:zz:zz:zz:zz",-50,-45,10,500,0,0,""]]}', b'{"t":"b","dev":[[null]]}',
    b'{"t":"z","dev":[["k",1,2,3,4,5,null,null,1,1,1]]}', b'{"t":"z","dev":7}',
    b'{"t":"pr","mac":null}', b'{"t":"pr","mac":["x"],"ssid":{"a":1}}',
    b'{"t":"fs","bins":7}', b'{"t":"fs","bins":[[1],[1,2,3,4,5]]}', b'{"t":"fd","mhz":"x"}',
    b'{"t":"ble","devs":1,"h":"bad"}', b'{"t":"ev","ev":[1]}', b'{"t":"ack"}', b'{"t":"ack","cmd":"hunt","hunt":7}',
    b'{"t":"ack","cmd":"deauth","deauth":[1]}', b'{"t":"ack","cmd":"sdread"}', b'{"t":"sdls","files":[1]}',
    b'{"t":"log"}', b'{"t":"err","msg":{"x":1}}', b'{"t":"log","msg":"sd card "}',
    b"P 1 2", b"P x y z w !!", b"S", b"S 1", b"M", b"M 1 2", b"MF", b"MF 1 2 3 4",
]


class MalformedLineTest(unittest.TestCase):
    def test_no_line_raises_out_of_handle_line(self):
        bw = make_bw()
        bw.start_capture()                         # exercises the P-line path too
        for raw in GARBAGE:
            with self.subTest(raw=raw):
                bw.handle_line(raw)                # must not raise
        feed(bw, HELLO_24)                         # and the host still works afterwards
        snap = bw.snapshot()
        self.assertEqual(snap["band"], "2.4g")
        self.assertEqual([c["ch"] for c in snap["channels"]], list(range(1, 14)))

    def test_snapshot_survives_garbage(self):
        bw = make_bw()
        for raw in GARBAGE:
            bw.handle_line(raw)
        bw.snapshot()                              # must not raise either (the HTTP thread calls it)

    def test_reader_session_survives_garbage(self):
        """End to end through reader(): garbage on the wire must not end the serial session (an exception
        there closes the port and silently stops a running capture)."""
        bw = make_bw(with_serial=False)
        bw.port_name = "fake-port"

        class Port(FakeSerial):
            def __init__(self, chunks):
                super().__init__()
                self.chunks = list(chunks)

            def read(self, n):
                if not self.chunks:
                    raise OSError("port gone")     # ends the session the way an unplug does
                return self.chunks.pop(0)

            def close(self):
                pass

        stream = b"\n".join(GARBAGE) + b"\n"
        hello = b'{"t":"hello","band":"154","chs":[11,12],"park":0,"cap":0,"heap":1}\n'
        # deliver in awkward pieces, including a split in the middle of the final good line
        data = stream + hello
        chunks = [data[i:i + 37] for i in range(0, len(data), 37)]
        port = Port(chunks)

        class Stop(Exception):
            pass

        def fake_sleep(_s):
            raise Stop()

        with mock.patch.object(bw, "open", return_value=port), mock.patch.object(bh.time, "sleep", fake_sleep):
            with self.assertRaises(Stop):
                bw.reader()
        self.assertEqual(bw.state["band"], "154", "the good line after the garbage was never processed")
        errors = [l for l in bw.state["log"] if l.startswith("serial error")]
        self.assertEqual(errors, ["serial error: port gone"])   # only the deliberate end of the session
        self.assertEqual(port.lines()[1], "info")
        self.assertTrue(port.lines()[0].startswith("time "))


class SendTest(unittest.TestCase):
    def test_send_collapses_newlines(self):
        bw = make_bw()
        self.assertTrue(bw.send("info\nreboot\r"))
        self.assertEqual(bw.ser.lines(), ["info reboot"])

    def test_send_without_port(self):
        self.assertFalse(make_bw(with_serial=False).send("info"))


if __name__ == "__main__":
    unittest.main()
