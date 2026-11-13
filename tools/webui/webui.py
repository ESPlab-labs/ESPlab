#!/usr/bin/env python3
"""ESPlab web UI — the dashboard service that runs on the machine itself.

On real hardware this is served by the coordinator board from pooled flash
(M6); on a dev machine (and tonight, as proof) it fronts one board's serial
console. Same HTTP surface either way: you point a browser at the machine
and see the whole cluster — nodes, jobs, storage — and can drive it.

Stdlib only: no pip, no framework, runs on anything."""
import json, os, subprocess, threading, queue, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/../.."
VB = ROOT + "/build/vboard"
PORT = int(os.environ.get("ESPLAB_WEB_PORT", "8080"))

# ---------- the board this service runs "on" ----------
# A real deployment: the board's UART. Here: a vboard process's console.
# The service is the machine's front door either way.
board_args = ["--id", "09", "--chip", "s3", "--cores", "2", "--ram", "512",
              "--flash", "8", "--console"]

class BoardConsole:
    def __init__(self, args):
        self.p = subprocess.Popen([VB] + args,
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  cwd=ROOT, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.lock = threading.Lock()

    def _reader(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\n"))

    def cmd(self, command, wait=0.8):
        with self.lock:
            self.p.stdin.write(command + "\n")
            self.p.stdin.flush()
            time.sleep(wait)
            out = []
            while True:
                try:
                    out.append(self.q.get_nowait())
                except queue.Empty:
                    return out

    def drain_silent(self, wait=0.3):
        time.sleep(wait)
        while True:
            try:
                self.q.get_nowait()
            except queue.Empty:
                return

# ---------- console -> structured views (parsers stay dumb on purpose) ----------
def parse_nodes(lines):
    nodes = []
    for l in lines:
        parts = l.split()
        if len(parts) >= 6 and len(parts[0]) == 12 and all(c in "0123456789abcdef" for c in parts[0]):
            nodes.append({"id": parts[0], "chip": parts[1], "cores": parts[2],
                          "ram": parts[3], "flash": parts[4], "state": parts[5]})
    return nodes

def parse_jobs(lines):
    jobs = []
    for l in lines:
        if l.startswith("#"):
            f = l.split()
            jobs.append({"id": f[0].lstrip("#"), "name": f[1] if len(f) > 1 else "?",
                         "kind": f[2] if len(f) > 2 else "?",
                         "state": f[3] if len(f) > 3 else "?",
                         "chunks": " ".join(f[4:]) if len(f) > 4 else ""})
    return jobs

def parse_status(lines):
    s = {"role": "?", "coord": "?", "boards": "?", "rx": 0, "tx": 0, "drops": 0}
    for l in lines:
        if l.startswith("role="):
            for kv in l.split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    s[k] = v
            s["boards"] = s.get("boards_up", s["boards"])
    return s

# ---------- machine singleton ----------
board = BoardConsole(board_args)
board.drain_silent(1.0)          # let it boot and join/win its election
BOARD_URL_ID = "09"

def machine_snapshot():
    st = parse_status(board.cmd("status"))
    nodes = parse_nodes(board.cmd("nodes"))
    jobs = parse_jobs(board.cmd("jobs"))
    log = board.cmd("log")
    return {"status": st, "nodes": nodes, "jobs": jobs, "log": log}

# ---------- the web service ----------
PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>ESPlab</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
 body { font-family: system-ui, sans-serif; background:#0d1117; color:#e6edf3;
        margin: 0 auto; max-width: 880px; padding: 1rem; }
 h1 { font-size: 1.3rem; } h1 span { color:#58a6ff; }
 h2 { font-size: .95rem; color:#8b949e; margin: 1.2rem 0 .3rem; }
 .grid { display:grid; grid-template-columns: 1fr 1fr; gap:.6rem; }
 .card { background:#161b22; border:1px solid #30363d; border-radius:8px; padding:.7rem; }
 .card b { color:#58a6ff; }
 table { width:100%; border-collapse:collapse; font-size:.85rem; }
 td,th { text-align:left; padding:.25rem .4rem; border-bottom:1px solid #21262d; }
 .ok { color:#3fb950; } .warn { color:#d29922; }
 button, input, select { background:#21262d; color:#e6edf3; border:1px solid #30363d;
        border-radius:6px; padding:.45rem .7rem; font-size:.9rem; }
 button { cursor:pointer; } button:hover { background:#30363d; }
 pre { background:#010409; border:1px solid #21262d; border-radius:8px;
       padding:.7rem; font-size:.78rem; overflow-x:auto; white-space:pre-wrap; }
 .row { display:flex; gap:.5rem; flex-wrap:wrap; margin:.3rem 0; }
 @media (max-width:640px){ .grid{grid-template-columns:1fr;} }
</style></head><body>
<h1>ESPlab <span>— many boards, one computer</span></h1>
<div id="app">connecting…</div>
<script>
async function api(path, opts) { const r = await fetch(path, opts);
  return r.json(); }
async function refresh() {
  const d = await api('/api/all');
  const s = d.status || {};
  const role = (s.role||'?').toUpperCase();
  const el = document.getElementById('app');
  let nodes = (d.nodes||[]).map(n =>
    `<tr><td><b>${n.id.slice(-2)}</b></td><td>${n.chip}</td><td>${n.cores}c</td>`+
    `<td>${n.ram}</td><td>${n.flash}</td><td class="${n.state==='up'?'ok':'warn'}">${n.state}</td></tr>`).join('');
  let jobs = (d.jobs||[]).map(j =>
    `<tr><td>${j.id}</td><td>${j.name}</td><td>${j.kind}</td>`+
    `<td class="${j.state==='DONE'?'ok':'warn'}">${j.state}</td><td>${j.chunks}</td></tr>`).join('');
  el.innerHTML = `
   <div class="grid">
    <div class="card"><h2>machine</h2>
      role <b>${role}</b> · brain ${String(s.coord||'?').slice(-2)} ·
      boards up <b>${s.boards||'?'}</b><br>
      rx ${s.rx||0} / tx ${s.tx||0} / drops ${s.drops||0}</div>
    <div class="card"><h2>act on the machine</h2>
      <div class="row">
        <button onclick="job('hello')">say hello</button>
        <button onclick="job('search')">search 0-100000, pred 17</button>
      </div>
      <div class="row">
        <input id="k" placeholder="key" size="8">
        <input id="v" placeholder="value" size="12">
        <button onclick="kvput()">store</button>
        <button onclick="kvget()">read</button>
      </div>
      <pre id="out" style="min-height:1.2em"></pre></div>
   </div>
   <h2>boards</h2><div class="card"><table>
     <tr><th>id</th><th>chip</th><th></th><th>ram</th><th>flash</th><th>state</th></tr>
     ${nodes||'<tr><td colspan=6>no boards seen yet</td></tr>'}</table></div>
   <h2>jobs</h2><div class="card"><table>
     <tr><th>#</th><th>name</th><th>kind</th><th>state</th><th>chunks</th></tr>
     ${jobs||'<tr><td colspan=5>no jobs</td></tr>'}</table></div>
   <h2>machine log</h2><pre>${(d.log||[]).join('\n')||'—'}</pre>`;
}
async function job(kind) {
  const body = kind==='hello' ? {kind:'hello'}
    : {kind:'search', lo:0, hi:100000, pred:17};
  const r = await api('/api/job', {method:'POST',
    headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
  document.getElementById('out').textContent = r.result || JSON.stringify(r);
  refresh();
}
async function kvput() {
  const k=document.getElementById('k').value, v=document.getElementById('v').value;
  if(!k) return;
  const r = await api('/api/kv', {method:'POST',
    headers:{'Content-Type':'application/json'}, body: JSON.stringify({k,v})});
  document.getElementById('out').textContent = r.result;
  refresh();
}
async function kvget() {
  const k=document.getElementById('k').value; if(!k) return;
  const r = await api('/api/kv?k='+encodeURIComponent(k));
  document.getElementById('out').textContent = r.result;
}
refresh(); setInterval(refresh, 3000);
</script></body></html>"""

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            b = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        elif u.path == "/api/all":
            self._json(machine_snapshot())
        elif u.path == "/api/kv":
            k = parse_qs(u.query).get("k", [""])[0]
            lines = board.cmd(f"get {k}") if k else ["?"]
            self._json({"result": "\n".join(lines) or "(no output)"})
        elif u.path == "/api/cmd":   # raw power escape hatch
            c = parse_qs(u.query).get("c", [""])[0]
            self._json({"result": board.cmd(c)})
        else:
            self._json({"error": "no such endpoint"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        if self.path == "/api/job":
            if req.get("kind") == "hello":
                r = board.cmd("submit hello", wait=1.5)
            else:
                lo = int(req.get("lo", 0)); hi = int(req.get("hi", 0))
                pred = int(req.get("pred", 0))
                r = board.cmd(f"submit search {lo} {hi} {pred}", wait=1.5)
            self._json({"result": " ".join(r)})
        elif self.path == "/api/kv":
            k = str(req.get("k", ""))[:24]; v = str(req.get("v", ""))[:80]
            if not k or " " in k:
                self._json({"result": "bad key"}, 400); return
            self._json({"result": " ".join(board.cmd(f"put {k} {v}"))})
        else:
            self._json({"error": "no such endpoint"}, 404)

if __name__ == "__main__":
    print(f"ESPlab web UI: http://0.0.0.0:{PORT}/  (board 09, console-backed)")
    try:
        ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        board.p.kill()
