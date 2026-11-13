#!/bin/bash
# build + run the band-level decoder test (frame 0 of grad.eod)
cd "$(dirname "$0")/.."
cc -Wall -O2 -Icore/inc tools/eod_bandtest.c core/src/vm.c -o build/eod_bandtest || exit 1
python3 tools/eod_frame.py /tmp/grad.eod 0 > /tmp/frame0.hex
python3 -c "
import binascii
h = open('/tmp/frame0.hex').read().strip()
b = binascii.unhexlify(h)
open('/tmp/frame0.bin','wb').write(b)
print('frame input bytes:', len(b))"
./build/eod_bandtest /tmp/eod_decode.bin /tmp/frame0.bin
