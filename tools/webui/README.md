# ESPlab WebUI

**The dashboard service for [ESPlab](https://github.com/TycoonCoder/ESPlab)
— many boards, one computer.**

Point a browser at the machine and see the whole cluster: boards, jobs,
storage, machine log — and drive it. ESP On Demand
([ESP-On-Demand](https://github.com/TycoonCoder/ESP-On-Demand)), the video
service, runs on top of this UI: it needs this service's console-backed API
to reach the cluster.

## What it is

On real hardware this is served by the coordinator board from pooled flash
(M6). On a dev machine (and in CI) it fronts one virtual board's serial
console. Same HTTP surface either way:

| endpoint | what it does |
|---|---|
| `GET /` | dashboard page |
| `GET /api/all` | machine snapshot: status, boards, jobs, log |
| `GET /api/kv?k=key` | read from the machine's pooled storage |
| `POST /api/kv` | store a key/value |
| `POST /api/job` | submit a job (hello, search) |
| `GET /api/frame?job=ID` | **ESP On Demand**: fetch a frame job's decoded bands |
| `GET /api/cmd?c=...` | raw console escape hatch (powers everything above) |

Stdlib only: no pip, no framework, runs on anything with Python 3.

## Running it

```bash
git clone https://github.com/TycoonCoder/ESPlab && cd ESPlab && make
python3 tools/webui/webui.py          # dashboard on :8080
```

The service spawns its own virtual board (id 09) which joins the machine —
start other vboards or flash real boards and they appear in the dashboard.

With ESP On Demand running elsewhere:

```bash
ESPLAB_EOD_URL=http://<machine>:8090 python3 tools/webui/webui.py
# the dashboard then links to the video service
```

## Console-backed by design

The service drives the machine **through the same serial console a human
types into** — `status`, `nodes`, `jobs`, `submit ...`, `put`, `get`,
`jobdump`. That is deliberate: the web UI has no privileged path, cannot
do anything the serial front panel can't, and every parser stays trivial.
When a new command exists, the web UI gets it for free via `/api/cmd`.
