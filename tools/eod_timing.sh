#!/bin/bash
# honest timing check: wall-clock each /api/frame fetch on a fresh service
set -e
cd /home/joshua/projects/esp32-cluster
export ESPLAB_VB_PORT=7912
export ESPLAB_WEB_PORT=8093
python3 tools/webui/webui.py > /tmp/eod_t_webui.log 2>&1 &
WPID=$!
sleep 6
python3 tools/eod_serve.py --webui http://127.0.0.1:8093 --clip /tmp/grad.eod \
        --port 8094 > /tmp/eod_t_eod.log 2>&1 &
EPID=$!
sleep 4
for n in 0 1 2 3; do
  curl -s -m 90 "http://127.0.0.1:8094/api/frame?n=$n" -o /tmp/f$n.json \
       -w "frame $n wall=%{time_total}s " || true
  python3 -c "import json;d=json.load(open('/tmp/f$n.json'));print('reported_ms=',d.get('ms'),'bytes=',len(d.get('pixels',[])))"
done
echo "--- metrics:"; curl -s -m 15 http://127.0.0.1:8094/metrics
echo; echo "--- killing services"
kill $EPID $WPID 2>/dev/null || true
sleep 1
pkill -f "webui.py" 2>/dev/null || true
exit 0
