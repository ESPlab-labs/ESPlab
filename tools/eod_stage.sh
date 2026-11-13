#!/bin/bash
# sanity: eod_serve.py parses, copy into the standalone repo staging dir
set -e
cd /home/joshua/projects/esp32-cluster
python3 -c "import ast; ast.parse(open('tools/eod_serve.py').read()); print('eod_serve syntax OK')"
cp tools/eod_serve.py /tmp/eod-repo/
grep -q "ESPlab-WebUI" tools/eod/README.md && echo "eod README links OK"
exit 0
