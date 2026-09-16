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

# Apple Silicon without Rosetta: Arduino's bundled ctags is x86_64-only; tools/ctags wraps universal-ctags.
if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] && ! /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
  command -v brew >/dev/null 2>&1 && brew list --versions universal-ctags >/dev/null 2>&1 || brew install universal-ctags
fi

arduino-cli config init --overwrite >/dev/null 2>&1 || true
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json >/dev/null 2>&1 || true
arduino-cli core update-index
arduino-cli core install esp32:esp32        # 3.3.x or newer: ESP32-C5 support
arduino-cli lib install "lvgl@9.3.0"

python3 -c "import serial" 2>/dev/null || python3 -m pip install --user pyserial

echo
echo "Toolchain ready."
arduino-cli core list | grep -E "esp32:esp32" || true
arduino-cli lib list | grep -E "^lvgl" || true
echo "Next: ./build.sh --upload   (plug the board in first)"
