#!/usr/bin/env bash
# Build Bandwatch 5G for the Waveshare ESP32-C5-LCD-1.47 with arduino-cli.
# Usage: ./build.sh            (compile only)
#        ./build.sh --upload   (compile + flash; auto-detects the serial port, or set PORT=/dev/cu.usbmodemXXXX)
set -euo pipefail
cd "$(dirname "$0")"

SKETCH="$PWD/bandwatch"
FQBN="esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=no_ota,FlashSize=4M,PSRAM=disabled,UploadSpeed=460800"
BUILD_DIR="$PWD/build"

# LVGL picks up the sketch-local lv_conf.h through LV_CONF_PATH (absolute path, quoted for the preprocessor).
LV_CONF="$SKETCH/lv_conf.h"
EXTRA="-DLV_CONF_PATH=\"$LV_CONF\""

# Arduino's bundled ctags is x86_64-only; tools/ctags wraps native universal-ctags instead (no Rosetta needed).
CTAGS_DIR="$PWD/tools/ctags"

args=(compile --fqbn "$FQBN" --build-path "$BUILD_DIR" \
      --build-property "runtime.tools.ctags.path=$CTAGS_DIR" \
      --build-property "compiler.cpp.extra_flags=$EXTRA" \
      --build-property "compiler.c.extra_flags=$EXTRA" \
      --warnings default "$SKETCH")

if [[ "${1:-}" == "--upload" ]]; then
  PORT="${PORT:-$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)}"
  if [[ -z "$PORT" ]]; then
    echo "No ESP32 serial port found. Plug the board in (USB) or set PORT=/dev/cu.xxx" >&2
    exit 1
  fi
  echo "Uploading to $PORT"
  args+=(--upload --port "$PORT")
fi

arduino-cli "${args[@]}"

if [[ "${1:-}" == "--upload" ]]; then
  # The ESP32-C5 USB-Serial-JTAG "hard reset" esptool does after flashing is only a core reset: it does
  # not re-sample the BOOT strap. If the strap was latched low (first flash after holding BOOT), the chip
  # sits in ROM download mode ("waiting for download") until a full system reset. Detect that and trigger a
  # watchdog reset from the ROM loader, which is a full reset. Otherwise leave the running app alone.
  ESPTOOL="$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1)"
  sleep 2
  STATE="$(python3 - "$PORT" <<'PY'
import sys, time
try:
    import serial
except ImportError:
    print("unknown"); sys.exit()
p = serial.Serial(); p.port = sys.argv[1]; p.baudrate = 115200; p.timeout = 0.3
p.dtr = True; p.rts = True   # asserted, no edge: does not reset a running board
try:
    p.open(); end = time.time() + 3; buf = b""
    while time.time() < end: buf += p.read(4096)
    print("download" if b"waiting for download" in buf else "running")
except Exception:
    print("unknown")
PY
)"
  if [[ "$STATE" == "download" && -x "$ESPTOOL" ]]; then
    "$ESPTOOL" --chip esp32c5 --port "$PORT" --before no-reset --after watchdog-reset run >/dev/null 2>&1 \
      && echo "Chip was stuck in download mode; reset into the new firmware." \
      || echo "Chip is in download mode and the automatic reset failed: press RESET on the board."
  else
    echo "Firmware is running (state: $STATE)."
  fi
fi
