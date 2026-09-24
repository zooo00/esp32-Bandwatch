#!/usr/bin/env bash
# Build Bandwatch 5G for the Waveshare ESP32-C5-LCD-1.47 with arduino-cli.
# Usage: ./build.sh                      (compile only)
#        ./build.sh --upload             (compile + patch + flash; PORT=/dev/cu.usbmodemXXXX to pick the port)
#        ./build.sh --upload --no-patch  (compile + flash the stock image, deauth will not transmit)
#
# The raw-TX patch (tools/deauth/patch_raw_tx.py, docs/DEVELOPER.md section 11) is applied by default: without
# it esp_wifi_80211_tx refuses deauth/disassoc subtypes and the attack transmits nothing. It edits the linked
# image, not the toolchain, so only this firmware is affected and every build must be patched again.
set -euo pipefail
cd "$(dirname "$0")"

SKETCH="$PWD/bandwatch"
FQBN="esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=4M,PSRAM=disabled,UploadSpeed=460800"
BUILD_DIR="$PWD/build"

UPLOAD=0
PATCH=1
for a in "$@"; do
  case "$a" in
    --upload)   UPLOAD=1 ;;
    --no-patch) PATCH=0 ;;
    *) echo "Unknown argument: $a" >&2; exit 2 ;;
  esac
done

# LVGL picks up the sketch-local lv_conf.h through LV_CONF_PATH (absolute path, quoted for the preprocessor).
LV_CONF="$SKETCH/lv_conf.h"
EXTRA="-DLV_CONF_PATH=\"$LV_CONF\""

# Arduino's bundled ctags is x86_64-only; tools/ctags wraps native universal-ctags instead (no Rosetta needed).
CTAGS_DIR="$PWD/tools/ctags"

arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_DIR" \
  --build-property "runtime.tools.ctags.path=$CTAGS_DIR" \
  --build-property "compiler.cpp.extra_flags=$EXTRA" \
  --build-property "compiler.c.extra_flags=$EXTRA" \
  --warnings default "$SKETCH"

if [[ "$UPLOAD" == "0" ]]; then
  [[ "$PATCH" == "0" ]] && echo "Note: --no-patch only affects flashing; nothing was flashed."
  exit 0
fi

PORT="${PORT:-$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)}"
if [[ -z "$PORT" ]]; then
  echo "No ESP32 serial port found. Plug the board in (USB) or set PORT=/dev/cu.xxx" >&2
  exit 1
fi

APP="$BUILD_DIR/bandwatch.ino.bin"
STOCK="$BUILD_DIR/bandwatch.ino.stock.bin"

restore_stock() {   # put the unpatched image back so the build dir is never left in a patched state
  # Must return 0 even when there is nothing to restore (--no-patch never writes $STOCK). Under
  # `set -e` a bare `[[ -f ... ]] && mv` returns 1 in that case, which killed the script at the
  # unconditional call below - after a successful flash, before the download-mode recovery.
  [[ -f "$STOCK" ]] && mv -f "$STOCK" "$APP"
  return 0
}

if [[ "$PATCH" == "1" ]]; then
  # Patch, then swap the patched image into place just for the upload. arduino-cli flashes whatever is at
  # bandwatch.ino.bin, so this keeps it to a single flash; the trap restores the stock image on any exit.
  python3 tools/deauth/patch_raw_tx.py
  cp "$APP" "$STOCK"
  trap restore_stock EXIT
  cp "$BUILD_DIR/bandwatch.ino.patched.bin" "$APP"
  echo "Uploading to $PORT (raw-TX patch applied)"
else
  echo "Uploading to $PORT (stock image: deauth will report ESP_ERR_INVALID_ARG and transmit nothing)"
fi

arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$BUILD_DIR" "$SKETCH"

restore_stock
trap - EXIT

# The ESP32-C5 USB-Serial-JTAG "hard reset" esptool does after flashing is only a core reset: it does
# not re-sample the BOOT strap. If the strap was latched low (first flash after holding BOOT), the chip
# sits in ROM download mode ("waiting for download") until a full system reset. Detect that and trigger a
# watchdog reset from the ROM loader, which is a full reset. Otherwise leave the running app alone.
# Both of the next two assignments are `|| true`-guarded: with `pipefail` set, a glob that matches
# nothing (or a missing python3) makes the assignment itself fail and `set -e` would abort here, i.e.
# exactly in the recovery step, right after a flash that worked.
ESPTOOL="$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1 || true)"
sleep 2
STATE="$(python3 - "$PORT" <<'PY' || true
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
STATE="${STATE:-unknown}"
if [[ "$STATE" == "download" && -x "$ESPTOOL" ]]; then
  "$ESPTOOL" --chip esp32c5 --port "$PORT" --before no-reset --after watchdog-reset run >/dev/null 2>&1 \
    && echo "Chip was stuck in download mode; reset into the new firmware." \
    || echo "Chip is in download mode and the automatic reset failed: press RESET on the board."
else
  echo "Firmware is running (state: $STATE)."
fi
