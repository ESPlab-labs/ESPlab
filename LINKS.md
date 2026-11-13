# ESPlab — many boards, one computer

Cluster of ESP32s that behaves as a single machine: pooled CPU, pooled
storage, a sandboxed task VM, and services that run **on the machine
itself**. Fully local — ESP-NOW only, no router, no cloud.

**Start here:**

- [README.md](README.md) — install → run → submit → verify
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the machine works
- [VM documentation site](https://esplab-labs.github.io/) — the
  task VM: instruction set, sandbox, assembler, worked examples
  (source in [docs/site](docs/site))

**Services that run on ESPlab (separate repos, linked):**

- [ESPlab-WebUI](https://github.com/ESPlab-labs/ESPlab-WebUI) — the
  machine's dashboard: boards, jobs, storage, log; console-backed,
  stdlib-only. In-repo copy: `tools/webui/`.
- [ESP-On-Demand](https://github.com/ESPlab-labs/ESP-On-Demand) — video
  on the cluster: every frame decoded by all boards in parallel.
  Requires the WebUI. Codec + decoder live here (`tools/eod_*`,
  `examples/eod_decode.s`); the service repo consumes them.

**Status:** v0.1.0 — 3-board virtual cluster verified end-to-end over real
UDP (election, work stealing, board death, fragmented 1.4 KB messages,
bytecode jobs, byte-exact frame decoding of a full 75-frame clip).
Real-board flash + OTA = the v1.0 line.
