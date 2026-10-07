#!/usr/bin/env bash
# Kept as an alias for old notes and muscle memory: since v2 became the default UI, this is the same as
# running bandwatch_host.py with no --ui flag (v2 at "/", classic at /classic). Use --ui classic for the old layout.
#   ./host/run-v2.sh            # auto-detects the serial port, serves http://127.0.0.1:8080
#   ./host/run-v2.sh --http 9000 --captures ~/bw-caps
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/bandwatch_host.py" --ui v2 "$@"
