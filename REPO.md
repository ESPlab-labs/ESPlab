# ESPlab — many boards, one computer

Cluster of ESP32s that behaves as a single machine: pooled CPU, pooled
storage, a sandboxed task VM, and services that run **on the machine
itself**. Fully local — ESP-NOW only, no router, no cloud.

**Quickstart, docs, architecture:** see [README.md](README.md) and the
[documentation site](https://tycooncoder.github.io/ESPlab/) (VM docs live there).

**Services that run on ESPlab:**

- [ESPlab-WebUI](https://github.com/TycoonCoder/ESPlab-WebUI) — the machine's
  dashboard: boards, jobs, storage, log; console-backed, stdlib-only.
- [ESP-On-Demand](https://github.com/TycoonCoder/ESP-On-Demand) — video on
  the cluster: every frame decoded by all boards in parallel. Requires the
  WebUI. This is the "can it do homelab things?" proof.

**Status:** v0.1.0 — 3-board virtual cluster verified end-to-end over real
UDP (election, work stealing, board death, fragmented 1.4 KB messages,
bytecode jobs, byte-exact frame decoding of a full 75-frame clip).
Real-board flash + OTA = the v1.0 line.
