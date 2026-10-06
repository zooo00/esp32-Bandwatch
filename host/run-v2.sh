#!/usr/bin/env bash
# Bandwatch host with the new dashboard (dashboard2.html) at "/". The classic UI stays one click away at /classic.
#   ./host/run-v2.sh            # auto-detects the serial port, serves http://127.0.0.1:8080
#   ./host/run-v2.sh --http 9000 --captures ~/bw-caps
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/bandwatch_host.py" --ui v2 "$@"
