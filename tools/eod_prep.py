#!/usr/bin/env python3
"""eod_prep — turn any video into an EOD clip for ESP On Demand.

Uses ffmpeg if available (any input video), else synthesizes test frames.
Output: <name>.eod (video) plus a frames-dump for pixel-truth testing."""
import subprocess, sys, os, struct

W, H, FPS = 64, 48, 10
DUR = 7.5                      # seconds

def main():
    if len(sys.argv) < 3:
        print("usage: eod_prep.py <input video> <out-prefix> [--synth]")
        return 1
    src, prefix = sys.argv[1], sys.argv[2]
    root = os.path.dirname(os.path.abspath(__file__)) + "/.."
    enc = root + "/build/eod_enc"
    raw = f"/tmp/{os.path.basename(prefix)}.rgb"
    if "--synth" in sys.argv or not os.path.exists(src):
        print("synthesizing test clip (gradient + moving bars)")
        frames = bytearray()
        for t in range(int(FPS * DUR)):
            for y in range(H):
                for x in range(W):
                    r = (x * 4 + t * 3) % 256
                    g = (y * 5 + t) % 256
                    b = ((x + y + t * 2) * 2) % 256
                    if ((x // 8) % 2) == 0:
                        r //= 2; g //= 2; b //= 2
                    frames += bytes((r, g, b))
        open(raw, "wb").write(frames)
    else:
        # any video -> raw rgb frames at our resolution/fps
        subprocess.run(["ffmpeg", "-y", "-i", src, "-f", "rawvideo",
                        "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                        "-r", str(FPS), "-t", str(DUR), raw], check=True)
    r = subprocess.run([enc, raw, f"{W}x{H}", prefix + ".eod", str(FPS)],
                       capture_output=True, text=True)
    print(r.stdout.strip() or r.stderr.strip())
    rawsz = os.path.getsize(raw); encsz = os.path.getsize(prefix + ".eod")
    print(f"source {rawsz}B -> {encsz}B over {DUR}s = {encsz/DUR/1024:.1f} KB/s "
          f"({rawsz/encsz:.1f}x compression)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
