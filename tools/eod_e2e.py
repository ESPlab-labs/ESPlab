#!/usr/bin/env python3
"""eod_e2e.py — end-to-end ESP On Demand test on a live virtual cluster.

Spawns 3 vboards on an isolated UDP group port, submits every frame of an
EOD clip as a frame job through a worker console (forwarding exercised),
waits for each job, then compares the machine's decoded band outputs with
a ground-truth decode done here in Python. Prints timing per frame."""
import os, sys, subprocess, time, struct, threading, queue, json

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
VB = ROOT + "/build/vboard"
PORT = os.environ.get("EOD_VB_PORT", "7900")
CLIP = os.environ.get("EOD_CLIP", "/tmp/grad.eod")
ONLY = int(os.environ.get("EOD_FRAMES", "0"))   # 0 = all frames

class Console:
    def __init__(self, name, args):
        self.name = name
        self.p = subprocess.Popen([VB] + args,
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  cwd=ROOT, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\n"))

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                return out

def cmd(con, command, wait=1.0):
    con.send(command)
    time.sleep(wait)
    return con.drain()

def eod_frames(path):
    b = open(path, "rb").read()
    assert b[:4] == b"EOD1"
    n = b[6] | (b[7] << 8)
    off = 8
    frames = []
    for _ in range(n):
        npairs = b[off] | (b[off+1] << 8)
        rle = b[off+2:off+2+npairs*2]
        frames.append(rle)
        off += 2 + npairs*2
    return frames

def ground_truth(rle, nchunks=12, pix=3072):
    idx = bytearray()
    for i in range(0, len(rle), 2):
        cnt = rle[i]; v = rle[i+1]
        idx += bytes([v]) * ((cnt >> 1) + 1)
    assert len(idx) == pix, f"decode len {len(idx)} != {pix}"
    return [bytes(idx[c*pix//nchunks:(c+1)*pix//nchunks]) for c in range(nchunks)]

def parse_jobdump(lines):
    chunks = {}
    for l in lines:
        if l.startswith("chunk ") and "incomplete" not in l:
            f = l.split(maxsplit=2)
            if len(f) >= 3:
                chunks[int(f[1])] = chunks.get(int(f[1]), b"") + bytes.fromhex(f[2])
    return chunks

def main():
    frames = eod_frames(CLIP)
    if ONLY:
        frames = frames[:ONLY]
    code = open("/tmp/eod_decode.bin", "rb").read()
    code_hex = code.hex()
    print(f"clip: {len(frames)} frames, decoder {len(code)}B, group port {PORT}")

    os.environ["ESPLAB_VB_PORT"] = PORT
    a = Console("A", ["--id", "16", "--cores", "2", "--ram", "290", "--flash", "4", "--console"])
    time.sleep(1.0)
    b = Console("B", ["--id", "17", "--chip", "s3", "--cores", "2", "--ram", "512", "--flash", "8", "--console"])
    time.sleep(5.0)
    c = Console("C", ["--id", "18", "--cores", "2", "--ram", "290", "--flash", "4", "--console"])
    time.sleep(3.0)

    fails = 0
    t_start = time.time()
    per_frame = []
    for fi, rle in enumerate(frames):
        payload = (struct.pack("<IHHHHHH", 0, len(rle), 3072, 0, 0, 0, fi) + rle).hex()
        # submit from the LAST board (worker) -> forward path gets exercised
        ack = cmd(c, f"submit frame {code_hex} {payload} 12", wait=1.0)
        if fi == 0:
            print("[worker ack]", ack[:4])
            print("[coord jobs]", cmd(a, "jobs", wait=0.8)[:4])
            print("[coord log]", cmd(a, "log", wait=0.8)[:8])
        # find the job id from the jobs listing (any state — decodes are fast)
        jobid = None
        for _ in range(30):
            lines = cmd(a, "jobs", wait=0.6)
            for l in lines:
                f = l.split()
                if l.startswith("#") and len(f) > 2 and f[2] == "frame":
                    jobid = f[0].lstrip("#")
            if jobid:
                break
            time.sleep(0.4)
        if not jobid:
            print(f"frame {fi}: NO JOB")
            fails += 1
            continue
        # wait for done
        for _ in range(40):
            lines = cmd(a, f"jobs", wait=0.5)
            done = any(l.startswith(f"#{jobid}") and "DONE" in l for l in lines)
            if done:
                break
            time.sleep(0.3)
        t0 = time.time()
        d = parse_jobdump(cmd(a, f"jobdump {jobid}", wait=1.2))
        gt = ground_truth(rle)
        if fi == 0 and d.get(0):
            print("[chunk0 got ]", d[0][:16].hex())
            print("[chunk0 want]", gt[0][:16].hex())
            print("[chunk0 lens]", len(d.get(0, b"")), len(gt[0]))
        ok = len(d) == 12 and all(d.get(k) == gt[k] for k in range(12))
        dt = time.time() - t_start
        per_frame.append(round(time.time() - t0, 2))
        if ok:
            print(f"frame {fi:3d}: PASS job #{jobid} 12/12 bands exact")
        else:
            got = sum(1 for k in range(12) if d.get(k) == gt[k])
            print(f"frame {fi:3d}: FAIL {got}/12 bands match, chunks={len(d)}")
            print("[coord log]", cmd(a, "log", wait=0.8)[-8:])
            fails += 1
    total = time.time() - t_start
    print(f"\n{len(frames)-fails}/{len(frames)} frames exact; wall {total:.1f}s "
          f"= {total/len(frames):.2f}s/frame; jobdump fetch ~{sum(per_frame)/max(1,len(per_frame)):.2f}s")
    a.p.kill(); b.p.kill(); c.p.kill()
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
