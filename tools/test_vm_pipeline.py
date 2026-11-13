#!/usr/bin/env python3
"""end-to-end proof for the vbasm -> cluster -> result pipeline.

Builds examples/count_a.s, submits it as a VM job to a running vboard
console, and compares the aggregate against ground truth."""
import subprocess, time, os, threading, queue, sys

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
VB = ROOT + "/build/vboard"
VAS = ROOT + "/build/vbasm"

# 1. assemble
r = subprocess.run([VAS, ROOT + "/examples/count_a.s", "-o", "/tmp/count_a.bin"],
                   capture_output=True, text=True)
print(r.stdout.strip() or r.stderr.strip())
if r.returncode: sys.exit(1)
code = open("/tmp/count_a.bin", "rb").read()
print(f"code: {len(code)} bytes")

# ground truth: 40 bytes 'aaxaaxaaxa' * 4 = 20 'a's, split into 4 chunks of 10
INPUT = b"aaxaaxaaxa" * 4
TRUE = sum(1 for b in INPUT if b == ord('a'))
print(f"input: {len(INPUT)} bytes, ground truth = {TRUE} 'a's")

class Console:
    def __init__(self, args):
        self.p = subprocess.Popen([VB] + args, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  cwd=ROOT, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
    def _reader(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\n"))
    def send(self, cmd):
        self.p.stdin.write(cmd + "\n"); self.p.stdin.flush()
    def drain(self, wait=1.0):
        time.sleep(wait)
        out = []
        while True:
            try: out.append(self.q.get_nowait())
            except queue.Empty: return out

def hexs(b): return b.hex()

a = Console(["--id", "03", "--cores", "2", "--ram", "290", "--flash", "4", "--console"])
time.sleep(5.0)                     # lone board: listen window + election
a.drain(0.5)
a.send(f"submit vm {hexs(code)} {hexs(INPUT)} 4")
tail = a.drain(4.0)
for l in tail: print("  ", l)
a.send("jobs")
for l in a.drain(1.0): print("  ", l)
a.send("job 1")
for l in a.drain(1.0): print("  ", l)
a.send("log")
for l in a.drain(1.0): print("  ", l)
a.p.kill()

ok = any(f"agg={TRUE}" in l for l in tail)
print("PIPELINE " + ("VERIFIED: correct aggregate from real cluster execution"
                     if ok else "FAILED"))
sys.exit(0 if ok else 1)
