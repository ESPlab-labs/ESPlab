#!/usr/bin/env python3
"""ESP On Demand — the video service for ESPlab.

Requires the ESPlab WebUI (the machine's dashboard service): this player
talks to the cluster exclusively through the WebUI's console-backed API.
Give it the WebUI's base URL and an .eod clip; it submits every frame as a
job, fetches each frame's decoded bands, and streams them to a browser
player at a pace set by the clip's fps.

  usage: eod_serve.py --webui http://127.0.0.1:8080 --clip /tmp/grad.eod \
                    [--port 8090] [--single] [--loop]

Endpoints:
  /            the player page
  /api/info    clip metadata (frames, fps, resolution, cluster boards up)
  /api/frame?n=N   decoded frame N as JSON {n, w, h, ms, pixels: [r,g,b,...]}
  /metrics     per-frame decode times (the pooling proof, in numbers)

If ESPLAB_VB_PORT is set, this process ALSO spawns its own dedicated
3-board virtual cluster on that UDP group port, so the service can be
demoed without a separately managed cluster. On real hardware you point
--webui at the board serving the dashboard and omit that."""
import argparse, json, os, subprocess, threading, time, struct, queue
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from urllib.request import urlopen

W, H, BANDS = 64, 48, 12
HERE = os.path.dirname(os.path.abspath(__file__))

def eod_read(path):
    b = open(path, "rb").read()
    assert b[:4] == b"EOD1", "not an EOD1 clip"
    fps = b[4]
    n = b[6] | (b[7] << 8)
    off, frames = 8, []
    while off < len(b) and len(frames) < n:
        npairs = b[off] | (b[off+1] << 8)
        frames.append(b[off+2:off+2+npairs*2])
        off += 2 + npairs*2
    return fps, frames

def decoder_bytes():
    # built by `make` in the ESPlab repo (examples/eod_decode.s -> eod_decode.bin)
    cands = [os.environ.get("EOD_DECODER", ""),
             HERE + "/../ESPlab/build/eod_decode.bin",   # standalone: sibling clone
             HERE + "/../build/eod_decode.bin"]          # in-repo tools layout
    for p in cands:
        if p and os.path.exists(p):
            return open(p, "rb").read()
    raise SystemExit("eod_decode.bin not found; in the ESPlab repo run "
                     "`make`, or point EOD_DECODER at it")

class Cluster:
    """Dedicated virtual cluster (dev/demo convenience) or none (real hw)."""
    def __init__(self, port):
        self.port = port
        self.procs = []
        self.consoles = []
        if not port:
            return
        env = dict(os.environ, ESPLAB_VB_PORT=port)
        vb = (os.environ.get("EOD_VBOARD")
              or HERE + "/../ESPlab/build/vboard"        # standalone: sibling clone
              or HERE + "/../build/vboard")
        for args in (["--id", "26", "--cores", "2", "--ram", "290", "--flash", "4"],
                     ["--id", "27", "--chip", "s3", "--cores", "2", "--ram", "512", "--flash", "8"],
                     ["--id", "28", "--cores", "2", "--ram", "290", "--flash", "4"]):
            self.procs.append(subprocess.Popen([vb] + args, env=env,
                                               cwd=os.path.dirname(vb)))
        time.sleep(7)   # boot + election

    def alive(self):
        return all(p.poll() is None for p in self.procs) if self.procs else True

def webui_cmd(webui, cmd, wait=1.0):
    url = webui.rstrip("/") + "/api/cmd?c=" + cmd.replace(" ", "%20")
    try:
        return json.loads(urlopen(url, timeout=30).read()).get("result", [])
    except Exception as e:
        return [f"webui error: {e}"]

class Decoder:
    """Submits frame jobs through the WebUI and fetches decoded bands."""
    def __init__(self, webui, code_hex):
        self.webui = webui
        self.code_hex = code_hex
        self.lock = threading.Lock()
        self.timings = []

    def submit(self, frame_hex, nchunks):
        with self.lock:
            t0 = time.time()
            r = webui_cmd(self.webui,
                          f"submit frame {self.code_hex} {frame_hex} {nchunks}",
                          wait=1.2)
            # the submit ack echoes the job id on the coordinator; find it
            jobid = None
            for _ in range(40):
                jobs = webui_cmd(self.webui, "jobs", wait=0.5)
                for l in jobs:
                    f = l.split()
                    if l.startswith("#") and len(f) > 2 and f[2] == "frame":
                        jobid = f[0].lstrip("#")
                if jobid:
                    break
                time.sleep(0.3)
            if not jobid:
                return None, r, time.time() - t0
            for _ in range(120):
                jobs = webui_cmd(self.webui, "jobs", wait=0.5)
                if any(l.startswith(f"#{jobid}") and "DONE" in l for l in jobs):
                    return jobid, r, time.time() - t0
                if any(l.startswith(f"#{jobid}") and "FAILED" in l for l in jobs):
                    return None, r, time.time() - t0
                time.sleep(0.3)
            return None, r, time.time() - t0

    def fetch(self, jobid):
        d = webui_cmd(self.webui, f"jobdump {jobid}", wait=1.5)
        chunks = {}
        for l in d:
            if l.startswith("chunk ") and "incomplete" not in l:
                f = l.split(maxsplit=2)
                if len(f) >= 3:
                    chunks[int(f[1])] = chunks.get(int(f[1]), b"") + bytes.fromhex(f[2])
        if len(chunks) != BANDS or any(len(chunks[k]) != 256 for k in chunks):
            return None
        return b"".join(chunks[k] for k in sorted(chunks))

    def decode(self, rle, fi):
        payload = (struct.pack("<IHHHHHH", 0, len(rle), W*H, 0, 0, 0, fi) + rle).hex()
        jobid, ack, dt = self.submit(payload, BANDS)
        if not jobid:
            return None, dt
        t0 = time.time()
        pix = self.fetch(jobid)
        return pix, dt + (time.time() - t0)

def expand(pix):
    """palette indices -> rgb triples for the browser canvas"""
    out = bytearray()
    for v in pix:
        r = (v >> 4) & 3; g = (v >> 2) & 3; b = v & 3
        out += bytes((r*85, g*85, b*85))
    return bytes(out)

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>ESP On Demand</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
 body { font-family: system-ui, sans-serif; background:#0d1117; color:#e6edf3;
        margin:0 auto; max-width:900px; padding:1rem; text-align:center; }
 h1 { font-size:1.2rem; } h1 span { color:#58a6ff; }
 canvas { image-rendering: pixelated; width:100%; max-width:640px;
          border:1px solid #30363d; border-radius:8px; background:#000; }
 .row { display:flex; gap:.5rem; justify-content:center; margin:.6rem 0; }
 button, select { background:#21262d; color:#e6edf3; border:1px solid #30363d;
        border-radius:6px; padding:.45rem .8rem; font-size:.9rem; }
 button { cursor:pointer; } button:hover { background:#30363d; }
 .stat { color:#8b949e; font-size:.85rem; }
</style></head><body>
<h1>ESP On Demand <span>— video, decoded by the cluster</span></h1>
<canvas id="v" width="64" height="48"></canvas>
<div class="row">
  <button id="play">play</button>
  <select id="rate">
    <option value="1">1x</option><option value="2">2x</option>
    <option value="4">4x</option>
  </select>
  <button id="loop">loop: on</button>
</div>
<div class="stat" id="stat">loading clip…</div>
<script>
const ctx = document.getElementById('v').getContext('2d');
let info = null, frames = [], t0 = 0, playing = false, looping = true;
async function load() {
  info = await (await fetch('/api/info')).json();
  document.getElementById('stat').textContent =
    `${info.frames} frames @ ${info.fps}fps · boards up ${info.boards} · ` +
    `decode ${info.decode_ms_avg} ms/frame (12 bands in parallel)`;
  frames = new Array(info.frames).fill(null);
  predecode();
}
async function predecode() {
  for (let i = 0; i < info.frames; i++) {
    if (frames[i]) continue;
    const f = await (await fetch('/api/frame?n=' + i)).json();
    if (f.pixels) frames[i] = new Uint8Array(f.pixels);
  }
  document.getElementById('stat').textContent += ' · ready';
  draw(0); t0 = performance.now(); playing = true;
}
function draw(i) {
  const px = frames[i % info.frames];
  if (!px) return;
  const img = ctx.createImageData(64, 48);
  for (let p = 0; p < 64*48; p++) {
    img.data[p*4]   = ((px[p] >> 4) & 3) * 85;
    img.data[p*4+1] = ((px[p] >> 2) & 3) * 85;
    img.data[p*4+2] = (px[p] & 3) * 85;
    img.data[p*4+3] = 255;
  }
  ctx.putImageData(img, 0, 0);
  document.getElementById('stat').textContent =
    `${info.frames} frames @ ${info.fps}fps · frame ${i % info.frames} · ` +
    `decode ${info.decode_ms_avg} ms/frame`;
}
function tick() {
  if (playing) {
    const i = Math.floor((performance.now() - t0) / (1000 / info.fps));
    draw(i);
    if (i >= info.frames && !looping) playing = false;
  }
  requestAnimationFrame(tick);
}
document.getElementById('play').onclick = () => {
  playing = !playing; t0 = performance.now();
};
document.getElementById('loop').onclick = (e) => {
  looping = !looping; e.target.textContent = 'loop: ' + (looping ? 'on' : 'off');
};
load(); requestAnimationFrame(tick);
</script></body></html>"""

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _send(self, body, ctype="application/json", code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            self._send(PAGE.encode(), "text/html; charset=utf-8")
        elif u.path == "/api/info":
            self._send(json.dumps(SRV.info()).encode())
        elif u.path == "/api/frame":
            n = int(parse_qs(u.query).get("n", ["0"])[0])
            pix, ms = SRV.frame(n)
            if pix is None:
                self._send(json.dumps({"error": "decode failed", "ms": ms}).encode(), code=502)
            else:
                self._send(json.dumps({"n": n, "w": W, "h": H, "ms": round(ms, 1),
                                       "pixels": list(expand(pix))}).encode())
        elif u.path == "/metrics":
            self._send(json.dumps(SRV.metrics()).encode())
        else:
            self._send(b'{"error":"no such endpoint"}', code=404)

class Service:
    def __init__(self, webui, clip, single=False):
        fps, frames = eod_read(clip)
        self.fps, self.frames, self.single = fps, frames, single
        code = decoder_bytes()
        self.dec = Decoder(webui, code.hex())
        self.cache = {}
        self.lock = threading.Lock()

    def info(self):
        # boards up, from the WebUI's snapshot of the machine
        try:
            st = json.loads(urlopen(self.dec.webui.rstrip("/") + "/api/all",
                                    timeout=15).read())["status"]
            boards = st.get("boards_up", "?")
        except Exception:
            boards = "?"
        with self.lock:
            times = [ms for _, ms in self.cache.values()]
        avg = round(sum(times) / len(times), 1) if times else None
        return {"frames": len(self.frames), "fps": self.fps, "w": W, "h": H,
                "boards": boards, "decode_ms_avg": avg}

    def frame(self, n):
        n %= len(self.frames)
        with self.lock:
            if n in self.cache:
                return self.cache[n][0], self.cache[n][1]
        rle = self.frames[n]
        pix, ms = self.dec.decode(rle, n)
        with self.lock:
            if pix:
                self.cache[n] = (pix, ms)
                if self.single and len(self.cache) >= len(self.frames):
                    pass  # keep serving from cache
        return pix, ms

    def metrics(self):
        with self.lock:
            return {"frames_decoded": len(self.cache),
                    "per_frame_ms": {str(k): round(v[1], 1) for k, v in sorted(self.cache.items())}}

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--webui", required=True, help="ESPlab WebUI base URL")
    ap.add_argument("--clip", required=True, help=".eod clip to serve")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--single", action="store_true", help="decode each frame once, then serve from cache")
    ap.add_argument("--own-cluster", action="store_true",
                    help="spawn a dedicated 3-board virtual cluster (demo mode)")
    ap.add_argument("--vb-port", default="", help="UDP group port for --own-cluster")
    a = ap.parse_args()
    if a.own_cluster:
        Cluster(a.vb_port or "7910")
    SRV = Service(a.webui, a.clip, single=a.single)
    print(f"ESP On Demand: http://0.0.0.0:{a.port}/  (webui {a.webui}, "
          f"{len(SRV.frames)} frames @ {SRV.fps}fps)")
    ThreadingHTTPServer(("0.0.0.0", a.port), Handler).serve_forever()
