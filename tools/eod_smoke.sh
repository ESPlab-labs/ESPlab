#!/bin/bash
# EOD service smoke: own webui (port 8091) + own cluster (UDP 7911) +
# eod_serve (port 8092), then fetch /api/info and frame 0 through the
# full chain (webui console -> cluster job -> decoded pixels).
set -e
cd /home/joshua/projects/esp32-cluster
export ESPLAB_VB_PORT=7911
export ESPLAB_WEB_PORT=8091
python3 tools/webui/webui.py > /tmp/eod_smoke_webui.log 2>&1 &
WPID=$!
sleep 6
python3 tools/eod_serve.py --webui http://127.0.0.1:8091 --clip /tmp/grad.eod \
        --port 8092 --single > /tmp/eod_smoke_eod.log 2>&1 &
EPID=$!
sleep 4
echo "--- /api/info:"
curl -s -m 20 http://127.0.0.1:8092/api/info
echo
echo "--- /api/frame?n=0 (first 120 chars):"
curl -s -m 60 "http://127.0.0.1:8092/api/frame?n=0" | head -c 120
echo
echo "--- done; killing services"
kill $EPID $WPID 2>/dev/null || true
sleep 1
pkill -f "vboard.*--id 26" 2>/dev/null || true
pkill -f "vboard.*--id 27" 2>/dev/null || true
pkill -f "vboard.*--id 28" 2>/dev/null || true
exit 0
