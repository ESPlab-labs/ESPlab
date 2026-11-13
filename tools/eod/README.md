# ESP On Demand

**Video on ESPlab — a tiny "Plex" for a cluster of ESP32 boards.**

ESP On Demand (EOD) is the proof that an ESPlab machine — a handful of
ESP32s acting as one computer — can do real media work. A 5–10 second clip
is encoded once on a PC into a custom format (`EOD1`), then *every frame is
decoded by the whole cluster*: each frame becomes a job, each job is chopped
into 12 pixel bands by the machine, and every band is decoded in parallel by
a sandboxed bytecode task running on whichever board is free. The browser
just paints the results.

It requires and runs on top of the [ESPlab WebUI](https://github.com/TycoonCoder/ESPlab-WebUI)
(the machine's dashboard service) — all cluster access goes through it.

## Why a cluster plays video better than one ESP32

| | single ESP32 | ESPlab machine (3 boards) |
|---|---|---|
| decode parallelism | 1 core, 12 bands serially | 12 bands across all boards |
| frame decode (vboard sim) | ~44 ms band-work × 12 | one job, bands in parallel |
| what the radio carries | — | 1.4 KB in (RLE), 256 B out per band |

The measured numbers from the live simulation live in `/metrics` while the
service runs (per-frame decode ms, from real job submits through the
machine's coordinator).

## The format (EOD1)

Chosen so the *decoder is trivial and the radio carries the least*:

- 64×48 frames, one **byte per pixel** in a 64-color palette (rgb222).
- Horizontal **RLE**: a frame is `pairs`; a pair is `(run-1)<<1` then the
  palette index. Typical clip: **10–12 KB/s** — a 7.5 s clip is ~80 KB.
- A frame fits in **one radio message** (1.4 KB reassembled), so a frame
  job is one submit; the machine does all slicing.

```
tools/eod_prep.py movie.mp4 clip        # any video -> clip.eod (needs ffmpeg)
tools/eod_prep.py x x --synth           # or a synthetic test clip
```

## Running it

```bash
# in the ESPlab repo: build the machine + decoder bytecode
git clone https://github.com/TycoonCoder/ESPlab && cd ESPlab && make

# 1. start the WebUI (the machine's front door)
python3 tools/webui/webui.py &                 # dashboard on :8080

# 2. start ESP On Demand on top of it
python3 tools/eod_serve.py --webui http://127.0.0.1:8080 \
        --clip /tmp/grad.eod --port 8090
```

Open `http://<machine>:8090/` — press play. Every frame you see was decoded
by the cluster while you watched.

### With real boards

Flash the boards with ESPlab firmware (see the main repo), point `--webui`
at whichever board runs the dashboard, and play. On hardware the WebUI
speaks the coordinator's serial console; EOD never talks to boards directly.

## Layout

- `eod_serve.py` — the service: clip cache, job submission, frame API, player page
- `README.md` — this file
- The codec (encoder `eod_enc.c`, header `eod_codec.h`), the decoder
  assembly (`examples/eod_decode.s`), and the end-to-end test
  (`eod_e2e.py`) live in the main ESPlab repo, because they are part of the
  machine's test surface.

## Status

- end-to-end proven on a 3-board virtual cluster over real UDP: every frame
  of the test clip decodes byte-exact vs. a ground-truth Python decoder
- external storage (SD/flash pool) is the next step: the clip cache and the
  EOD1 reader are already separated from the web layer, so a storage backend
  drops in behind `eod_read()`.
