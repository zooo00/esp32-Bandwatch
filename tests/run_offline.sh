#!/usr/bin/env bash
# Run the offline test tiers: 1 = host (fast, anywhere), 2 = firmware (compiles with ./build.sh, needs
# arduino-cli; never flashes). Extra args go to unittest, e.g. tests/run_offline.sh -v
# The hardware tier (tests/device) is separate and needs BANDWATCH_PORT - see tests/README.md.
set -uo pipefail
cd "$(dirname "$0")/.."
status=0
echo "== tier 1: host (tests/host) =="
python3 -m unittest discover -s tests/host "$@" || status=1
echo "== tier 2: firmware (tests/firmware) =="
python3 -m unittest discover -s tests/firmware "$@" || status=1
exit $status
