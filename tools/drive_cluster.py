#!/usr/bin/env python3
"""drive a 3-board virtual ESPlab cluster, one command at a time.

Every command is followed by an explicit drain so each response line is
attributable to its command. Ends by printing the coordinator's log."""
import subprocess, time, os, threading, queue

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
VB = ROOT + "/build/vboard"

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
    lines = con.drain()
    print(f"[{con.name}] $ {command}")
    for l in lines:
        print(f"    {l}")

def main():
    a = Console("A/coord?", ["--id", "06", "--cores", "2", "--ram", "290", "--flash", "4", "--console"])
    time.sleep(1.0)
    b = Console("B", ["--id", "07", "--chip", "s3", "--cores", "2", "--ram", "512", "--flash", "8", "--console"])
    time.sleep(5.0)
    c = Console("C", ["--id", "08", "--cores", "2", "--ram", "290", "--flash", "4", "--console"])
    time.sleep(3.0)

    cmd(a, "status"); cmd(b, "status"); cmd(c, "status")
    cmd(c, "submit search 0 10000 13", wait=3.0)
    cmd(a, "jobs")
    cmd(a, "job 1")
    cmd(c, "jobs")
    cmd(c, "submit hello", wait=3.0)
    cmd(a, "jobs")
    cmd(a, "job 2")
    cmd(a, "put greeting hello from the machine")
    cmd(a, "get greeting")
    cmd(a, "log")

    a.p.kill(); b.p.kill(); c.p.kill()

if __name__ == "__main__":
    main()
