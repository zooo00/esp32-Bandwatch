#!/usr/bin/env bash
# One-shot toolchain setup for this project on a fresh machine (macOS with Homebrew, or Linux with
# arduino-cli already on PATH). Safe to re-run.
#
#   ./setup.sh          install arduino-cli, the ESP32 core (3.3.x, has ESP32-C5), LVGL 9.3, pyserial
#   ./build.sh          compile
#   ./build.sh --upload compile + flash the board
#   python3 host/bandwatch_host.py   live dashboard + pcap capture on http://127.0.0.1:8080
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v arduino-cli >/dev/null 2>&1; then
  if command -v brew >/dev/null 2>&1; then
    brew install arduino-cli
  else
    echo "arduino-cli not found. Install it: https://arduino.github.io/arduino-cli/latest/installation/" >&2
    exit 1
  fi
fi

# Apple Silicon: Arduino's bundled ctags is x86_64-only, so build.sh always routes ctags through
# tools/ctags, which wraps universal-ctags. That wrapper is used whether or not Rosetta is installed,
# so universal-ctags is a hard dependency on arm64 — not a Rosetta-only fallback.
if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
  if ! brew list --versions universal-ctags >/dev/null 2>&1; then
    brew install universal-ctags
  fi
fi

arduino-cli config init --overwrite >/dev/null 2>&1 || true
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json >/dev/null 2>&1 || true
arduino-cli core update-index
# Pinned on purpose: the driver-internal deauth path in bandwatch.cpp pokes hard-coded offsets inside this
# core's prebuilt libnet80211.a (see docs/DEVELOPER.md section 9). Bump it deliberately, then re-verify them.
ESP32_CORE_VERSION="${ESP32_CORE_VERSION:-3.3.11}"
arduino-cli core install "esp32:esp32@${ESP32_CORE_VERSION}"
arduino-cli lib install "lvgl@9.3.0"

# Homebrew's Python is PEP 668 "externally managed", where a plain --user install is refused. There is no
# brew formula for pyserial, so fall back to --break-system-packages (it only touches the user site dir).
if ! python3 -c "import serial" 2>/dev/null; then
  python3 -m pip install --user pyserial \
    || python3 -m pip install --user --break-system-packages pyserial
fi

echo
echo "Toolchain ready."
arduino-cli core list | grep -E "esp32:esp32" || true
arduino-cli lib list | grep -E "^lvgl" || true
echo "Next: ./build.sh --upload   (plug the board in first)"
