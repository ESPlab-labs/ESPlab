#!/bin/bash
# machine-time honesty check: time the pure decode path once via the
# e2e harness's jobdump fetch (no webui polling sleeps in the middle)
set -e
cd /home/joshua/projects/esp32-cluster
EOD_FRAMES=2 EOD_VB_PORT=7913 timeout 240 python3 tools/eod_e2e.py 2>&1 | grep -E "PASS|FAIL|frames"
exit 0
