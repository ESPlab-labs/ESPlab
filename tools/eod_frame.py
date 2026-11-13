#!/usr/bin/env python3
"""eod_frame.py — grab one frame's RLE as hex for `submit frame`.

usage: eod_frame.py <clip.eod> <frame-index> [--dump]
Prints the frame's RLE bytes as hex (the submit-frame payload); --dump
adds a python dict with header info for debugging."""
import struct, sys

def main():
    if len(sys.argv) < 3:
        print("usage: eod_frame.py <clip.eod> <frame-index> [--dump]")
        return 1
    path, idx = sys.argv[1], int(sys.argv[2])
    b = open(path, "rb").read()
    assert b[:4] == b"EOD1", "not an EOD clip"
    fps = b[4]
    nframes = b[6] | (b[7] << 8)
    if idx >= nframes:
        print(f"frame {idx} out of range (0..{nframes-1})")
        return 1
    off = 8
    fi = 0
    while fi < idx:
        npairs = b[off] | (b[off + 1] << 8)
        off += 2 + npairs * 2
        fi += 1
    npairs = b[off] | (b[off + 1] << 8)
    rle = b[off + 2: off + 2 + npairs * 2]
    assert len(rle) == npairs * 2, "truncated clip"
    # VM task input: 16-byte header then the frame's RLE
    hdr = struct.pack("<IHHHHHH", 0, npairs * 2, 64 * 48, 0, 0, 0, idx)
    payload = hdr + rle
    if "--dump" in sys.argv:
        print({"frame": idx, "of": nframes, "fps": fps,
               "rle_len": len(rle), "input_len": len(payload)})
    print(payload.hex())

if __name__ == "__main__":
    sys.exit(main())
