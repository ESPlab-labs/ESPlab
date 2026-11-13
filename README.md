# ESPlab

![license](https://img.shields.io/badge/license-MIT-blue)
![platform](https://img.shields.io/badge/platform-ESP32%20%2F%20linux-orange)
![tests](https://img.shields.io/badge/tests-passing-brightgreen)
![version](https://img.shields.io/badge/version-v0.1.1-blue)

**Many boards, one computer.** ESPlab turns a pile of ESP32 boards into a
single small computer. Flash every board with the same firmware, plug them
into USB power, and they find each other, elect a brain, and start acting as
one machine: pooled CPU, pooled storage, one console.

It is designed to be *small*, not distributed: you submit work like you would
to a single board ("search 0 to 100000") and the machine slices it across
boards itself. Losing a board costs a retry, not the job.

## Features

- **No router, no network, no internet** — boards talk directly by radio
  (ESP-NOW). Your homelab stays your homelab.
- **One firmware for every board** — roles are elected, not flashed. New
  boards join in seconds, mixed chips (classic/S2/S3/C3) included; stronger
  boards automatically take bigger workloads.
- **The machine chops the work** — a job like `search 0 100000 13` is split
  into chunks and distributed automatically. You never hand-split anything.
- **Fault tolerance** — a dead worker's chunk is redone elsewhere; if the
  brain dies, the machine re-elects and keeps going.
- **Task VM** — user tasks compile to a tiny deterministic bytecode that
  runs on any board, sandboxed with hard time/memory/output limits so a bad
  task dies alone. See the [VM documentation](https://esplab-labs.github.io/vm.html).
- **Pooled storage** — a read-mostly key/value store spread across boards;
  write on one board, read from any.

## Quick start (real boards)

One-time toolchain setup, then flash every board the same way — roles are
elected at run time, never flashed:

```bash
git clone https://github.com/ESPlab-labs/ESPlab && cd ESPlab
# ESP-IDF v5.2 install: see docs/HARDWARE.md Step 1 (one-time, ~10 min)
cd firmware
idf.py set-target esp32          # or esp32s2 / esp32s3 / esp32c3
idf.py -p /dev/ttyUSB0 flash monitor
```

The monitor shows the machine waking up:

```
ESPlab node up: aabbccddeeff
election gen=1 (no cluster heard)
I am the coordinator
```

Flash your other boards the same way (one port each). They join within
seconds and the stronger chip wins the election on merit. Then use the
machine from any board's console:

```text
(esplab) status                  # every board, with chip type
(esplab) submit search 0 10000 13
(esplab) jobs
(esplab) put greeting hello from the machine
(esplab) get greeting
```

**The complete hardware guide — toolchain, serial ports, mixed-chip fleets,
what the console should print, troubleshooting — is
[docs/HARDWARE.md](docs/HARDWARE.md).**

From the computer the boards are plugged into, add the dashboard and the
media service:

```bash
python3 tools/webui/webui.py                    # dashboard: http://localhost:8080
python3 tools/eod_serve.py --webui http://127.0.0.1:8080 \
        --clip clip.eod --port 8090             # video:    http://localhost:8090
```

No hardware yet? **[docs/TESTING.md](docs/TESTING.md)** runs the whole
machine on your PC with virtual boards — same core code, UDP instead of
radio.

## Writing your own tasks (the VM)

```text
cat > task.s <<'EOF'
# count input bytes equal to 'a' in this chunk, emit the count
  loadi r2, 'a'
  loadi r3, 0
loop:
  cmp  r4, r0, r1      # r4 = (r0 < slice_end)?
  jz   r4, done
  ldv  r5, r0          # r5 = input[r0]
  cmp  r6, r5, r2
  jnz  r6, skip
  add  r3, r3, 1
skip:
  add  r0, r0, 1
  jmp  loop
done:
  emit r3
  halt
EOF
tools/vbasm task.s -o task.bin
(esplab) submit vm task.bin < input.bin 4   # 4 chunks over the input
```

The full instruction set, sandbox rules, and worked examples are in the
[VM documentation](https://esplab-labs.github.io/vm.html).

## Related projects

- [ESPlab-WebUI](https://github.com/ESPlab-labs/ESPlab-WebUI) — the machine's
  dashboard service (console-backed)
- [ESP-On-Demand](https://github.com/ESPlab-labs/ESP-On-Demand) — a media
  server that runs on ESPlab: video decoded by the whole cluster
- [LINKS.md](LINKS.md) — full list of repos and docs

## Project layout

```
core/      the machine: node core, task VM, console (portable C, no OS deps)
host/      vboard: one node as a Linux process (UDP transport)
firmware/  the ESP32 board firmware (thin adapter over the same core)
tools/     vbasm assembler, cluster driver, web dashboard
docs/      ARCHITECTURE.md, HARDWARE.md (flashing), TESTING.md (emulator), MILESTONES.md
```

## Status

v0.1.1 — the machine exists and is verified. Election, work sharing,
board-death recovery, brain-death re-election, pooled storage, VM jobs, and
fragment reassembly are all covered by a deterministic simulator
(`core/tests`) and proven live on a 3-node virtual cluster over UDP.
Real-board flashing is documented and CI-verified; USB flashing on your
desk is the next proof, then OTA.

## License

[MIT](LICENSE)
