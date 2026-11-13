# ESP On Demand (in-tree service copy)

**The service itself lives in its own repo:
[TycoonCoder/ESP-On-Demand](https://github.com/TycoonCoder/ESP-On-Demand) —
including the [end-to-end walkthrough](https://github.com/TycoonCoder/ESP-On-Demand/blob/main/QUICKSTART.md)
(virtual boards, real boards, encoding a clip, playing video).**

This is the in-tree copy of the docs for the machine's test surface.

![license](https://img.shields.io/badge/license-MIT-blue)
![requires](https://img.shields.io/badge/requires-ESPlab%20WebUI-red)
![status](https://img.shields.io/badge/e2e-75%2F75%20frames%20byte--exact-brightgreen)

**A media server that runs on ESPlab** — a cluster of ESP32 boards acting as
one computer.

ESP On Demand (EOD) serves video from an ESPlab machine and uses the whole
cluster to decode it. A clip is encoded once on a PC into a compact format
(`EOD1`), then *every frame is decoded by the whole cluster*: each frame
becomes a job, each job is chopped into 12 pixel bands by the machine, and
every band is decoded in parallel by a sandboxed bytecode task running on
whichever board is free. The browser just paints the results.

It requires and runs on top of the
[ESPlab WebUI](https://github.com/TycoonCoder/ESPlab-WebUI) (the machine's
dashboard service) — all cluster access goes through it.

## How it works

```
clip.eod ──▶ EOD service ──▶ ESPlab machine ──▶ 12 band jobs, decoded
             (per frame)      (slices each        in parallel across
                              frame into bands)   all boards
                                    │
browser player ◀── reassembled bands + timing
```

- The **service** (this repo) never talks to boards directly. It submits
  `frame` jobs through the WebUI's console-backed API and serves the
  reassembled results to the player page.
- The **machine** does all slicing. One submit per frame; the coordinator
  hands each band to a free board as a sandboxed VM job.
- The **decoder** is a 136-byte bytecode program that runs identically on
  every board (classic/S2/S3/C3 — the VM is deterministic, so any board can
  decode any band).

## The format (EOD1)

Chosen so the decoder is trivial and the radio carries the least:

- 64×48 frames, one **byte per pixel** in a 64-color palette (rgb222).
- Horizontal **RLE**: a frame is `(run-1)<<1, index` pairs. Typical clip:
  **10–12 KB/s** — a 7.5 s clip is ~80 KB.
- A frame fits in **one radio message** (1.4 KB reassembled), so a frame job
  is one submit; the machine does all slicing.

```bash
tools/eod_prep.py movie.mp4 clip        # any video -> clip.eod (needs ffmpeg)
tools/eod_prep.py x x --synth           # or a synthetic test clip
```

## Running it

Requires the [ESPlab WebUI](https://github.com/TycoonCoder/ESPlab-WebUI) and
an ESPlab machine (virtual boards or real ones):

```bash
# 1. the machine + WebUI (see the ESPlab repo's quick start)
git clone https://github.com/TycoonCoder/ESPlab && cd ESPlab && make
python3 tools/webui/webui.py &                 # dashboard on :8080

# 2. ESP On Demand on top of it
python3 tools/eod_serve.py --webui http://127.0.0.1:8080 \
        --clip /tmp/grad.eod --port 8090
```

Open `http://<machine>:8090/` and press play. Per-frame decode times are
reported live at `/metrics`.

With real boards: flash them with ESPlab firmware (see the
[main repo](https://github.com/TycoonCoder/ESPlab#readme)), point `--webui`
at the machine running the dashboard, and play. On hardware the WebUI speaks
the coordinator's serial console; EOD never talks to boards directly.

## Layout

- `eod_serve.py` — the service: clip cache, job submission, frame API, player page
- `README.md` — this file

The codec (encoder `eod_enc.c`, header `eod_codec.h`), the decoder assembly
(`examples/eod_decode.s`), and the end-to-end test (`eod_e2e.py`) live in the
[ESPlab repo](https://github.com/TycoonCoder/ESPlab), because they are part
of the machine's test surface.

## Status

- end-to-end proven on a 3-board virtual cluster over real UDP: every frame
  of the test clip decodes byte-exact vs. a ground-truth Python decoder
- external storage (SD/flash pool) is the next step: the clip cache and the
  EOD1 reader are already separated from the web layer, so a storage backend
  drops in behind `eod_read()`

## License

[MIT](LICENSE)
