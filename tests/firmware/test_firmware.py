"""Firmware tier (offline; needs arduino-cli + the ESP32 core from ./setup.sh, never touches a board).

    python3 -m unittest discover -s tests/firmware -v

* test_build_static_ram: runs ./build.sh (compile only - never --upload), then fails if "Global variables use N
  bytes" exceeds the ceiling in static_ram_ceiling.json. The compile itself also enforces the layout
  static_asserts in bandwatch/devices.h (WifiDev == 64 B, BleDev == 48 B, Dev154 == 24 B) and the channel-table
  assert in bandwatch_core.h: if one fires, the build fails and so does this test.
  Set BANDWATCH_BUILD_LOG=<file> to parse an existing build log instead of compiling again.
* test_lcd_strings_ascii: every string literal in the LCD code is plain ASCII. The Montserrat fonts LVGL is
  built with lack glyphs such as U+00B7 (middle dot) or U+2014 (em dash); they render as boxes on the panel.
"""
import json
import os
import re
import shutil
import subprocess
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CEILING_FILE = os.path.join(os.path.dirname(__file__), "static_ram_ceiling.json")
LCD_SOURCES = [os.path.join(REPO, "bandwatch", "lcd_ui.cpp")]
GLOBALS_RE = re.compile(r"Global variables use (\d+) bytes")
FLASH_RE = re.compile(r"Sketch uses (\d+) bytes")


def string_literals(src):
    """Yield (line, body) for each "..." literal in C/C++ source, skipping comments and char literals.
    Raw strings are not used in this code base and are not handled."""
    i, n, line = 0, len(src), 1
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
        elif c in "\"'":
            j, start = i + 1, line
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    line += 1
                j += 1
            if c == '"':
                yield start, src[i + 1:j]
            i = j + 1
        else:
            i += 1


def non_ascii_problems(body):
    """Non-ASCII characters, or escapes that produce non-ASCII bytes/code points, in one literal body."""
    bad = [ch for ch in body if ord(ch) > 0x7E]
    for m in re.finditer(r"\\(x[0-9a-fA-F]+|u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[0-7]{3})", body):
        esc = m.group(1)
        val = int(esc, 8) if esc[0].isdigit() else int(esc[1:], 16)
        if val > 0x7E:
            bad.append("\\" + esc)
    return bad


class LcdStringsTest(unittest.TestCase):
    def test_scanner_catches_known_bad_forms(self):
        src = '// comment · ok\nlv_label_set_text(l, "a · b"); x = "\\xC2\\xB7"; y = "ok\\n"; c = \'·\';'
        found = [(ln, non_ascii_problems(b)) for ln, b in string_literals(src)]
        self.assertEqual([p for _, p in found if p], [["·"], ["\\xC2", "\\xB7"]])

    def test_lcd_strings_ascii(self):
        problems = []
        for path in LCD_SOURCES:
            with open(path, encoding="utf-8") as f:
                src = f.read()
            for line, body in string_literals(src):
                bad = non_ascii_problems(body)
                if bad:
                    problems.append(f"{os.path.relpath(path, REPO)}:{line}: {body!r} has {bad}")
        self.assertEqual(problems, [], "LCD text must be ASCII (Montserrat has no glyph for these):\n" +
                         "\n".join(problems))


class BuildTest(unittest.TestCase):
    def build_output(self):
        log = os.environ.get("BANDWATCH_BUILD_LOG")
        if log:
            with open(log, encoding="utf-8", errors="replace") as f:
                return f.read()
        if not shutil.which("arduino-cli"):
            self.skipTest("arduino-cli not installed (run ./setup.sh)")
        # Compile only: build.sh flashes only with --upload, and we never pass it.
        p = subprocess.run(["./build.sh"], cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=3600)
        if p.returncode != 0:
            self.fail("./build.sh failed (exit %d); a static_assert in devices.h/bandwatch_core.h fails here too."
                      "\n--- last 60 lines ---\n%s" % (p.returncode, "\n".join(p.stdout.splitlines()[-60:])))
        return p.stdout

    def test_build_static_ram(self):
        out = self.build_output()
        m = GLOBALS_RE.search(out)
        self.assertIsNotNone(m, "no 'Global variables use N bytes' line in the build output")
        used = int(m.group(1))
        with open(CEILING_FILE) as f:
            ceiling = json.load(f)["global_bytes_ceiling"]
        flash = FLASH_RE.search(out)
        print(f"\n  static RAM: {used} B (ceiling {ceiling} B, margin {ceiling - used:+d} B)"
              + (f"; flash {int(flash.group(1))} B" if flash else ""))
        self.assertLessEqual(used, ceiling,
                             f"static RAM {used} B exceeds the recorded ceiling {ceiling} B by {used - ceiling} B. "
                             "Find the regression, or raise global_bytes_ceiling in "
                             "tests/firmware/static_ram_ceiling.json on purpose (CLAUDE.md rule 4).")


if __name__ == "__main__":
    unittest.main()
