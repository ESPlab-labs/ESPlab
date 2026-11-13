# ESPlab — many boards, one computer

ESPlab turns a pile of ESP32 boards into a **single small computer**. Flash
every board with the same firmware, plug them into USB power, and they find
each other, elect a brain, and start acting as one machine: pooled CPU,
pooled storage, one console. Small — a real computer's smallest thumbnail —
but *one* computer, not a distributed system you have to think about.

- **No router, no network, no internet.** Boards talk directly by radio
  (ESP-NOW).
- **One firmware for every board.** Roles are elected, not flashed. Plug a
  board in and it joins in seconds — mixed chips (classic/S2/S3/C3) work
  together; stronger boards automatically take bigger workloads.
- **The machine chops the work.** A job says "search 0 to 100000" — the
  machine slices it across the boards itself. You never hand-split anything.
- **Survives losing boards.** A dead worker's chunk is redone elsewhere. If
  the brain dies, the machine re-elects a brain and keeps going.
- **A task VM.** User tasks compile to a tiny bytecode blob that runs on any
  board, sandboxed so a bad task dies without taking the machine with it.

**Related repos and docs:** [LINKS.md](LINKS.md) — the WebUI and ESP On
Demand services, and the VM documentation site.

**Status: v0.1 — the machine exists and is verified.** Election, work
sharing, board-death recovery, brain-death re-election, pooled storage, VM
jobs, and fragment reassembly are all tested by a deterministic simulator
(`core/tests`) and proven live on a 3-node virtual cluster over UDP. Real
board firmware and OTA are the next milestones (see `docs/MILESTONES.md`).

## Run it right now (no hardware needed)

Every part of ESPlab is portable C, so a "board" can be a Linux process
(`host/vboard.c`) — the same core code the real boards run, with a wire-shaped
UDP transport. Three commands:

```
make            # builds build/vboard, build/test_cluster, build/vbasm
make test       # deterministic cluster simulator — must print ALL TESTS PASSED
```

Then, in three terminals (or let `tools/drive_cluster.py` do it for you):

```
./build/vboard --id 1 --cores 2 --ram 290 --flash 4 --console
./build/vboard --id 2 --chip s3 --cores 2 --ram 512 --flash 8 --console
./build/vboard --id 3 --cores 2 --ram 290 --flash 4 --console
```

Boards announce, elect (the S3 wins on merit), and hot-join. From *any*
board's console — a worker's is as good as the brain's:

```
status                        # what the machine looks like from here
submit search 0 10000 13      # count multiples of 13 below 10000, pooled
jobs                          # every job on the machine, from any board
job 1                         # per-chunk detail
put greeting hello from the machine
get greeting                  # pooled storage: write on one board, read anywhere
log                           # the machine's own log
```

### The dashboard (a service running on the machine)

```
python3 tools/webui/webui.py   # http://localhost:8080
```

See the whole machine in a browser — boards, jobs, storage, machine log — and
submit work through it. This is the proof that ESPlab runs the kind of things
homelabs run: the dashboard is itself a service *on* the machine.

The WebUI is now its own repo: [TycoonCoder/ESPlab-WebUI](https://github.com/TycoonCoder/ESPlab-WebUI)
(this copy stays in-tree as the in-repo service). On top of it runs
**[ESP On Demand](https://github.com/TycoonCoder/ESP-On-Demand)** — video
decoded by the whole cluster, every frame split across the boards.
Docs: [the VM and the machine](https://tycooncoder.github.io/ESPlab/).

### Writing your own tasks (the VM)

```
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

The VM is deliberately tiny (18 opcodes, deterministic, sandboxed: memory
caps, cycle budgets, no I/O). Compile on your computer, ship the blob to the
machine over serial, any board runs it.

## Layout

```
core/    the machine: node core, task VM, console (portable C, no OS deps)
host/    vboard: one node as a Linux process (UDP transport)
firmware/  the ESP32 board firmware (thin adapter over the same core)
tools/   vbasm assembler, cluster driver, web dashboard
docs/    ARCHITECTURE.md (the design), MILESTONES.md (the build order)
```

## License

MIT.
