# ESPlab

[![release](https://img.shields.io/github/v/release/ESPlab-labs/ESPlab)](https://github.com/ESPlab-labs/ESPlab/releases)
[![license](https://img.shields.io/github/license/ESPlab-labs/ESPlab)](LICENSE)
[![CI](https://github.com/ESPlab-labs/ESPlab/actions/workflows/build.yml/badge.svg)](https://github.com/ESPlab-labs/ESPlab/actions/workflows/build.yml)

Cluster firmware for ESP32 boards. Flash the same image onto as many boards
as you like — two or twenty, any mix of ESP32, S2, S3 and C3 — and they
behave as one computer: work submitted to any board is split across all of
them, and every board shares one storage pool and one console. Boards talk
to each other directly over ESP-NOW, so the cluster needs no router, no
Wi-Fi network and no internet.

[Documentation](https://esplab-labs.github.io) ·
[Hardware guide](docs/HARDWARE.md) ·
[Releases](https://github.com/ESPlab-labs/ESPlab/releases) ·
[Issues](https://github.com/ESPlab-labs/ESPlab/issues)

## Why ESPlab

| | |
|---|---|
| **One firmware, every role** | No coordinator/worker flashing decision. Boards elect their own brain at boot; the strongest board wins. |
| **Mixed fleets** | ESP32, S2, S3 and C3 boards work together. Bigger boards automatically take bigger workloads. |
| **Grows with your pile of boards** | Add or remove boards any time; a new board joins in seconds and work rebalances. |
| **Survives losses** | If a board dies mid-job its work is redone elsewhere. If the coordinator dies, a new one is elected. |
| **Sandboxed user tasks** | Programs run as bytecode in a small VM with hard time, memory and output limits — a bad task dies alone. |
| **Shared storage** | A key/value store pooled across boards: write on one board, read from any. |
| **Fully local** | ESP-NOW point-to-point radio. Nothing leaves your lab. |

## Install (real boards)

```bash
git clone https://github.com/ESPlab-labs/ESPlab
cd ESPlab/firmware
idf.py set-target esp32      # esp32 | esp32s2 | esp32s3 | esp32c3
idf.py -p /dev/ttyUSB0 flash monitor
```

Repeat for each board (one USB port each). The full walkthrough — toolchain
install, serial ports, mixed-chip fleets, troubleshooting — is in
[docs/HARDWARE.md](docs/HARDWARE.md).

Use the machine from any board's serial console:

```text
status                        # all boards, chip types, who coordinates
submit search 0 100000 13     # a job, split across every board
put greeting hello
get greeting                  # shared storage
```

The web dashboard and the ESP On Demand media server run on top of a
flashed cluster; see [ESPlab-WebUI](https://github.com/ESPlab-labs/ESPlab-WebUI)
and [ESP-On-Demand](https://github.com/ESPlab-labs/ESP-On-Demand).

## Trying it without boards

The same core runs on your PC: [docs/TESTING.md](docs/TESTING.md) covers the
test suite and the virtual-board emulator, and prebuilt host tools ship in
[Releases](https://github.com/ESPlab-labs/ESPlab/releases).

## Documentation

- [The task VM](https://esplab-labs.github.io/vm.html) — instruction set,
  sandbox rules, assembler, worked examples
- [Architecture](docs/ARCHITECTURE.md) — how election, scheduling and
  storage work
- [Milestones](docs/MILESTONES.md) — what exists and what's planned

## Status

Firmware builds for all four ESP32 targets in CI and passes the full host
test suite. USB flashing is documented today; OTA updates are planned next.

## License

[MIT](LICENSE)
