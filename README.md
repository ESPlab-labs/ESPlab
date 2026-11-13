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
  task dies alone. See the [VM documentation](https://tycooncoder.github.io/ESPlab/vm.html).
- **Pooled storage** — a read-mostly key/value store spread across boards;
  write on one board, read from any.

## Quick start

No hardware needed — a "board" can be a Linux process running the same core
code as the real firmware:

```bash
git clone https://github.com/TycoonCoder/ESPlab
cd ESPlab
make && make test          # deterministic cluster simulator, ALL TESTS PASSED
```

Bring up three boards and drive the machine from any of them:

```bash
./build/vboard --id 1 --cores 2 --ram 290 --flash 4 --console
./build/vboard --id 2 --chip s3 --cores 2 --ram 512 --flash 8 --console
./build/vboard --id 3 --cores 2 --ram 290 --flash 4 --console
```

```text
status                        # what the machine looks like from here
submit search 0 10000 13      # count multiples of 13 below 10000, pooled
jobs                          # every job on the machine, from any board
put greeting hello from the machine
get greeting                  # pooled storage: write on one, read anywhere
log                           # the machine's own log
```

Prebuilt host tools (`vboard`, `vbasm`, `eod_enc`) are in
[Releases](https://github.com/TycoonCoder/ESPlab/releases) — no compiler
needed.

### The dashboard

```bash
python3 tools/webui/webui.py   # http://localhost:8080
```

See the whole machine in a browser — boards, jobs, storage, log — and submit
work through it. The dashboard is itself a service *on* the machine.

### Writing your own tasks (the VM)

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
./build/vboard --id 9 --console
(esplab) submit vm task.bin < input.bin 4   # 4 chunks over the input
```

The full instruction set, sandbox rules, and worked examples are in the
[VM documentation](https://tycooncoder.github.io/ESPlab/vm.html).

## Real hardware

The `firmware/` tree is a thin ESP-IDF adapter over the same portable core;
CI builds it for real ESP32 targets on every push. Flashing is the same
image for every board — the machine elects roles itself. USB flashing and
OTA updates are the next milestones ([docs/MILESTONES.md](docs/MILESTONES.md)).

## Related projects

- [ESPlab-WebUI](https://github.com/TycoonCoder/ESPlab-WebUI) — the machine's
  dashboard service (console-backed)
- [ESP-On-Demand](https://github.com/TycoonCoder/ESP-On-Demand) — a media
  server that runs on ESPlab: video decoded by the whole cluster
- [LINKS.md](LINKS.md) — full list of repos and docs

## Project layout

```
core/      the machine: node core, task VM, console (portable C, no OS deps)
host/      vboard: one node as a Linux process (UDP transport)
firmware/  the ESP32 board firmware (thin adapter over the same core)
tools/     vbasm assembler, cluster driver, web dashboard
docs/      ARCHITECTURE.md (the design), MILESTONES.md (the build order)
```

## Status

v0.1.1 — the machine exists and is verified. Election, work sharing,
board-death recovery, brain-death re-election, pooled storage, VM jobs, and
fragment reassembly are all covered by a deterministic simulator
(`core/tests`) and proven live on a 3-node virtual cluster over UDP.

## License

[MIT](LICENSE)
