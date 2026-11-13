#!/usr/bin/env python3
"""submit a vbasm-built task to a single vboard; every step visible.

Spawns one board, waits for its election, feeds `submit vm <hexcode>
<hexinput> 4`, and reports per-chunk results vs ground truth."""
import subprocess, time, threading, queue, os, sys

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
VB = ROOT + "/build/vboard"
VAS = ROOT + "/build/vbasm"

# 1. assemble the example task
r = subprocess.run([VAS, ROOT + "/examples/count_a.s", "-o", "/tmp/count_a.bin"],
                   capture_output=True, text=True)
print(r.stdout.strip() or r.stderr.strip())
if r.returncode:
    sys.exit(1)
code_hex = open("/tmp/count_a.bin", "rb").read().hex()
repeats = int(os.environ.get("INPUT_REPEATS", "4"))
INPUT = b"aaxaaxaaxa" * repeats
input_hex = INPUT.hex()
truth = sum(1 for b in INPUT if b == ord('a'))
nboards = int(os.environ.get("BOARDS", "1"))
print(f"code {len(code_hex)//2}B, input {len(INPUT)}B, ground truth {truth}")

# nboards boards; extra workers join after the first is up
procs = []
p = subprocess.Popen([VB, "--id", "03", "--cores", "2", "--ram", "290",
                      "--flash", "4", "--console"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, cwd=ROOT, text=True, bufsize=1)
q = queue.Queue()
def rd():
    for line in p.stdout:
        q.put(line.rstrip("\n"))
threading.Thread(target=rd, daemon=True).start()
for b in range(2, 2 + nboards - 1):
    time.sleep(0.5)
    # c3 workers: weaker than the console board's classic chip, so the
    # console board keeps the coordinator role and its log ring is readable
    logf = open(f"/tmp/vb_{b:02d}.log", "w")
    procs.append(subprocess.Popen([VB, "--id", f"{b:02d}", "--chip", "c3",
                                   "--cores", "1", "--ram", "290",
                                   "--flash", "4"],
                                  stdin=subprocess.DEVNULL,
                                  stdout=logf,
                                  stderr=subprocess.STDOUT, cwd=ROOT))
    procs[-1].logf = logf
time.sleep(3.0)

def drain(t=1.0):
    time.sleep(t)
    out = []
    while True:
        try: out.append(q.get_nowait())
        except queue.Empty: return out

def send(c):
    p.stdin.write(c + "\n")
    p.stdin.flush()

time.sleep(5.0)
drain(0.3)
send("status")
print("status:", drain(1.0))
send(f"submit vm {code_hex} {input_hex} 4")
resp = drain(5.0)
print("submit resp:", resp)
send("jobs")
jobs_out = drain(1.0)
print("jobs:", jobs_out)
send("job 1")
print("job1:", drain(1.0))
send("log")
log_out = drain(1.0)
print("log:", log_out)
p.kill()
for q2 in procs:
    try: q2.logf.flush(); q2.logf.close(); q2.kill()
    except Exception: pass
# the job's truth lives on whichever board won the election — scan all
worker_logs = ""
for b in range(2, 2 + nboards - 1):
    try:
        worker_logs += open(f"/tmp/vb_{b:02d}.log").read()
    except Exception: pass
ok = any(f"agg={truth}" in l for l in resp + jobs_out) or \
     f"agg={truth}" in worker_logs
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
