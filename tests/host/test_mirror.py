"""Offline tests: LCD-mirror framing (M regions, MF frame markers, legacy firmware, held torn frames)."""
import base64
import unittest
from unittest import mock

from _support import bh, make_bw, feed, FakeClock


def m_line(x, y, w, h, fill=0xFFFF):
    px = fill.to_bytes(2, "little") * (w * h)
    return b"M %d %d %d %d " % (x, y, w, h) + base64.b64encode(px)


def pixel(bw, x, y):
    i = (y * bw.screen_w + x) * 2
    return int.from_bytes(bw.screen[i:i + 2], "little")


class MirrorTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock(1000.0)
        p = mock.patch.object(bh.time, "time", self.clock)
        p.start()
        self.addCleanup(p.stop)
        self.bw = make_bw()

    def test_framed_regions_publish_only_at_marker(self):
        bw = self.bw
        feed(bw, b"MF 0 1")                       # firmware with markers
        self.assertTrue(bw.screen_framed)
        seq0 = bw.screen_seq
        self.clock.advance(0.05)
        feed(bw, m_line(0, 0, 4, 2, 0x1234))
        feed(bw, m_line(10, 10, 2, 2, 0xABCD))
        self.assertEqual(bw.screen_seq, seq0, "a region must not be published before its frame marker")
        self.assertEqual(pixel(bw, 0, 0), 0)
        self.clock.advance(0.05)
        feed(bw, b"MF 1 1")
        self.assertEqual(bw.screen_seq, seq0 + 1)
        self.assertEqual(pixel(bw, 3, 1), 0x1234)
        self.assertEqual(pixel(bw, 11, 11), 0xABCD)
        self.assertTrue(bw.screen_status()["complete"])

    def test_marker_with_nothing_new_does_not_bump_seq(self):
        bw = self.bw
        feed(bw, b"MF 0 1")
        feed(bw, m_line(0, 0, 1, 1))
        feed(bw, b"MF 1 1")
        seq = bw.screen_seq
        feed(bw, b"MF 2 1")
        self.assertEqual(bw.screen_seq, seq)

    def test_legacy_firmware_without_markers_publishes_per_region(self):
        bw = self.bw
        feed(bw, m_line(0, 0, 2, 2, 0x0F0F))
        self.assertFalse(bw.screen_framed)
        self.assertEqual(bw.screen_seq, 1)
        self.assertEqual(pixel(bw, 1, 1), 0x0F0F)
        feed(bw, m_line(5, 5, 1, 1, 0x00F0))
        self.assertEqual(bw.screen_seq, 2)
        self.assertFalse(bw.screen_status()["framed"])

    def test_markers_stopping_falls_back_to_legacy(self):
        bw = self.bw
        feed(bw, b"MF 0 1")
        for _ in range(bh.MIRROR_LEGACY_REGIONS + 1):
            feed(bw, m_line(0, 0, 1, 1))
        self.assertFalse(bw.screen_framed)
        self.assertGreater(bw.screen_seq, 0)

    def test_torn_frame_is_held_then_shown(self):
        bw = self.bw
        feed(bw, b"MF 0 1")                       # publishes (pub_t = now) though nothing was dirty
        self.clock.advance(0.1)
        feed(bw, m_line(0, 0, 2, 2, 0x1111))
        self.clock.advance(0.1)
        feed(bw, b"MF 1 0")                       # torn, and the last publish was only 0.2 s ago: hold it
        self.assertEqual(pixel(bw, 0, 0), 0)
        self.assertEqual(bw.screen_stats["held"], 1)
        self.clock.advance(bh.MIRROR_HOLD_S)
        feed(bw, m_line(0, 0, 2, 2, 0x2222))
        feed(bw, b"MF 2 0")                       # still torn, but nothing shown for > MIRROR_HOLD_S: show it
        self.assertEqual(pixel(bw, 0, 0), 0x2222)
        st = bw.screen_status()
        self.assertIs(st["complete"], False)

    def test_held_torn_frame_replaced_by_next_complete_frame(self):
        bw = self.bw
        feed(bw, b"MF 0 1")
        feed(bw, m_line(0, 0, 1, 1, 0x1111))
        feed(bw, b"MF 1 0")                       # held
        self.assertEqual(pixel(bw, 0, 0), 0)
        feed(bw, m_line(1, 0, 1, 1, 0x2222))      # the device's repair
        feed(bw, b"MF 2 1")
        self.assertEqual((pixel(bw, 0, 0), pixel(bw, 1, 0)), (0x1111, 0x2222))
        self.assertIs(bw.screen_status()["complete"], True)

    def test_screen_status_complete_flag_not_clobbered_by_counter(self):
        """Regression: screen_status() merged **screen_stats, whose complete-frame counter was also called
        "complete" and silently replaced the bool the dashboard reads ("in sync" vs "repairing")."""
        bw = self.bw
        feed(bw, b"MF 0 1")
        feed(bw, m_line(0, 0, 1, 1))
        feed(bw, b"MF 1 1")                       # one complete frame: counter = 2
        self.clock.advance(bh.MIRROR_HOLD_S + 0.1)
        feed(bw, m_line(0, 0, 1, 1, 0x1))
        feed(bw, b"MF 2 0")                       # a torn frame published after the hold
        st = bw.screen_status()
        self.assertIs(st["complete"], False, "complete must be the published frame's flag, not a counter")
        self.assertEqual(st["complete_frames"], 2)
        self.assertEqual(st["frames"], 3)

    def test_stale_back_buffer_published_by_status_poll(self):
        bw = self.bw
        feed(bw, b"MF 0 1")
        feed(bw, m_line(0, 0, 1, 1, 0x7777))      # its MF marker is lost
        self.assertEqual(pixel(bw, 0, 0), 0)
        bw.screen_status()                        # too soon: still held
        self.assertEqual(pixel(bw, 0, 0), 0)
        self.clock.advance(bh.MIRROR_STALE_S + 0.01)
        st = bw.screen_status()
        self.assertEqual(pixel(bw, 0, 0), 0x7777)
        self.assertIs(st["complete"], False)

    def test_gap_counting_and_wrap(self):
        bw = self.bw
        feed(bw, b"MF 65534 1")
        feed(bw, b"MF 65535 1")
        feed(bw, b"MF 0 1")                       # 16-bit wrap is not a gap
        self.assertEqual(bw.screen_stats["gaps"], 0)
        feed(bw, b"MF 5 1")
        self.assertEqual(bw.screen_stats["gaps"], 1)

    def test_on_flag_follows_stream(self):
        bw = self.bw
        self.assertFalse(bw.screen_status()["on"])
        feed(bw, b"MF 0 1")
        self.assertTrue(bw.screen_status()["on"])
        self.clock.advance(3.5)
        self.assertFalse(bw.screen_status()["on"])

    def test_malformed_regions_rejected(self):
        bw = self.bw
        good = base64.b64encode(b"\xff\xff" * 4)
        for raw in (b"M 0 0 2 2 !!!notbase64",
                    b"M 0 0 2 2 " + base64.b64encode(b"\xff\xff" * 3),        # wrong length
                    b"M 171 0 2 2 " + good,                                  # off the right edge
                    b"M 0 319 2 2 " + good,                                  # off the bottom
                    b"M -1 0 2 2 " + good, b"M 0 0 0 0 ", b"M 0 0", b"M a b c d e",
                    b"MF", b"MF x 1", b"MF 1"):
            feed(bw, raw)
        self.assertEqual(bytes(bw.screen_back), bytes(len(bw.screen_back)))
        self.assertEqual(bw.screen_seq, 0)


if __name__ == "__main__":
    unittest.main()
