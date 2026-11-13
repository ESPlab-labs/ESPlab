# ESPlab — Architecture

*(Named ESPlab. The repo: github.com/ESPlab-labs/ESPlab.)*

## 1. Vision

Many small boards acting as **one computer**. You flash every board with the
same firmware, plug them into USB power, and they find each other and become a
single machine with pooled CPU, pooled storage, and one console. It is small —
a real computer's smallest thumbnail — but it is *one* computer, not a
distributed system you have to think about.

This project is for release on GitHub. It must work for someone with 3 classic
ESP32s, someone with 10 mixed boards (classic + S2 + S3 + C3), and someone who
plugs an 11th board in on a random Tuesday.

## 2. Hardware reality (design constraints, stated honestly)

- Classic ESP32: 2 cores @ 240 MHz, ~520 KB RAM total, ~290 KB usable heap
  with WiFi-class radio active, 4 MB flash (typical). S3: more RAM, optional
  PSRAM. C3: 1 core, less RAM.
- Radio: every board's radio is WiFi hardware. We use **ESP-NOW**: direct
  board-to-board datagrams, **no router, no network, no internet**. ~100 m
  line-of-sight, 1–2 ms latency, ~1 Mbps, **250-byte frames** (we chunk).
- Remote RAM access over the radio costs 1–3 ms per touch; local RAM costs
  nanoseconds. Therefore **RAM is never pooled transparently**. Workers share
  nothing at RAM level; they exchange *messages*. This is a feature: it is
  what makes the same design work from 3 boards to 30.
- Flash wears out (~10k–100k writes per sector). Storage design is
  write-rarely, wear-leveled (§8).

## 3. The One-Computer model

The cluster pretends to be a single desktop computer. The metaphor maps like
this and drives every API decision:

| Concept in the cluster | Desktop analogy |
| --- | --- |
| The whole fleet | The computer |
| Coordinator (elected board) | The kernel / motherboard |
| Every board, including the coordinator | A CPU core |
| ESP-NOW transport | The motherboard bus |
| Pooled flash | The hard disk |
| Serial cable into the coordinator | The power button + monitor + keyboard |

Consequences:

- There is **one console**: whatever board the user's USB cable is plugged
  into *is* the computer's front panel (§9).
- There is **one job list**, **one filesystem**, **one set of settings**.
  Users never address boards individually for normal work.
- **Failure model (user decision):** failure of one board is generally
  failure of the machine — like a dead core or a dead CPU in a desktop. The
  machine does not shard its identity to survive. Specifically:
  - Worker dies mid-task → the supervisor notices, and the task's chunk is
    rescheduled onto a live core (what an OS does with a dead core's thread).
    Tasks must be *pure* (§6) so re-running is safe.
  - **Coordinator dies → the whole machine is down.** Surviving boards detect
    the silence and hold a new election automatically (the machine "reboots
    its brain"). In-flight jobs restart from their chunk lists. No data loss
    beyond what an unfinished job implies.
  - Boards that come back (power blink, reboot) simply rejoin and resume
    pulling work.
- There is no consensus protocol, no quorum math, no per-object replication.
  The coordinator owns the authoritative state (job list, storage index,
  membership). Workers are stateless executors plus their local flash share.

## 4. Membership and the coordinator

- **One firmware image for all boards.** No roles at flash time.
- At boot every board listens for an existing cluster; if none, boards run a
  **bully-free simple election**: highest (uptime-stable score, then lowest
  MAC) wins. The winner is coordinator **and still executes tasks** like
  everyone else. Election re-runs only when the coordinator goes silent.
- **Hot join (user decision):** a board flashed with the image, powered,
  and in radio range announces itself; the coordinator adds it to the machine
  and it starts pulling work within seconds. No config, no restart.
- Each board announces a **capability card**: chip type, core count, RAM,
  flash size, PSRAM, task-slot count. The scheduler uses it to weight chunk
  sizes (an S3 gets bigger chunks than a classic) and the VM uses it to check
  a task blob fits before dispatching.

## 5. Transport

- ESP-NOW in a thin layer (`bus_*`) so a future transport (wired UART bus,
  newer radios) can be swapped in without touching anything above it.
- Framing: 250-byte ESP-NOW payload → our packet = [header: version, type,
  src, dst, seq, flags] + payload. Anything larger (task blobs, file blocks)
  is **chunked and reassembled** with per-chunk ACKs and a small resend
  window. One unacked retry, then the link layer reports the peer unreachable
  and upper layers react per §3's failure model.
- **Pairing/security (user decision):** the cluster must not be
  meddled-with. Setup: during first boot (or via serial `pair` command) every
  board gets the cluster's shared key; ESP-NOW's built-in encryption (PMK +
  per-peer LTK) is enabled. Boards without the key are ignored at the radio
  layer — they cannot join, submit, or read. The cluster never touches the
  internet; there is no path for it to do so.

## 6. Job engine

- A **job** is submitted over serial (later: web UI) as a spec:
  `{type, params, input, range}`. `type` is either a built-in native task
  (compiled into the firmware, fast) or a VM task (a uploaded bytecode blob,
  §7).
- **The machine does the chopping (user decision).** The spec declares
  the work as a *space* — a numeric range, a byte range of an input file, a
  key list — and the supervisor divides it into chunks sized by each core's
  capability card. Users never hand-split anything.
- **Pull scheduling (user decision):** cores ask the supervisor for the
  next chunk when free. Self-balancing across mixed hardware, and a slow
  board can only slow itself.
- **Purity rule:** tasks are pure functions of their input chunk — same
  input, same output, no side effects, no "turn on the light" tasks. This is
  what makes dead-core rescheduling (§3) safe. Enforced by convention in v1
  (documented loudly), by VM sandbox design (no I/O opcodes) for VM tasks.
- Results stream back chunk-by-chunk to the supervisor, which assembles them
  into the job's result object in the storage volume.

## 7. The task VM

Design decision: VM, accepting modest speed cost, because it is the only
format where one task blob runs on every chip type (classic, S2, S3, C3 —
Xtensa and RISC-V alike) and can even be re-dispatched to a different board
mid-job.

- **Format:** a compact register-based bytecode, fixed-width instructions,
  int32 + fixed-point arithmetic first (soft-float later), ~32 opcodes to
  start. No pointers, no arbitrary memory access, no syscalls except the
  sandbox's own (read chunk input, emit output, yield).
- **Sandbox:** each task gets a fixed memory pool (its slot), an instruction
  budget per timeslice, and zero I/O. A runaway or crashed task is killed at
  its timeslice boundary and reported; the machine keeps running.
- **Determinism:** identical bytecode + identical input = identical output on
  every chip. The VM core is written as portable C that also compiles on the
  dev machine, so it gets real unit tests and a host-side assembler/simulator
  without hardware in the loop.
- **Speed strategy:** the VM is for user logic (glue, algorithms, decision
  making). Hot loops that matter get *native accelerator ops* — built-in
  primitives the VM calls (hash, AES, math kernels) implemented in C per
  chip. Expect 2–5x slower than pure C for plain VM code; accelerators close
  the gap for the loops that matter.

## 8. Storage — the machine's disk

- Pooled flash across boards presents as **one filesystem with one
  namespace** (`/`), files chunked into blocks spread over boards' spare
  flash. The supervisor keeps the block index (which file, which blocks,
  which boards). 3 classic boards ≈ 6–7 MB usable volume after firmware and
  system partitions.
- **Write-rarely discipline:** the volume is for configs, task blobs, job
  results, logs — read-mostly data. It is never a scratch disk for
  intermediate computation (workers use their local RAM for that). Wear
  leveling spreads writes across each board's sectors.
- **Failure model per §3:** if a board holding blocks is down, those parts of
  the disk are offline; the machine reports which files are affected rather
  than pretending. Optional mirroring (keep 2 copies of each block) is a
  later *setting*, default off, documented honestly.
- **External storage adapters (user requirement):** a storage backend
  interface so a board with an SD card (or later, a networked NAS bridge)
  contributes a much bigger share of the disk. v1 ships the interface and an
  SD-card adapter stub as the example.

## 9. The console (serial first)

The coordinator exposes the machine over USB serial. Command sketch:

```
help                        — command list
status                      — machine overview: cores up, load, disk
nodes                       — each board: chip, RAM, flash, state
jobs                        — job list and states
submit type=... range=...   — submit a job
job <id>                    — job detail / results
put <file> / get <file> / ls — the machine's disk
pair                        — (re)pair a board with the cluster key
log                         — recent machine log
```

Serial-first gives the bare-bones, maximum-control experience users want;
the web UI (hosted by the coordinator) is a later, optional layer users can
flash on, not the default.

## 10. Firmware layout and updates

- **Partition plan from day one** (4 MB flash): two OTA-sized app slots
  (~1.6 MB each) + NVS + the board's storage-volume share. Designing this in
  at M1 avoids repartitioning pain when OTA lands.
- **OTA (required):** new firmware is a file in the machine's disk;
  the supervisor streams it to each board over ESP-NOW into the inactive
  slot, boards verify (hash), then the machine rolls over board-by-board with
  automatic rollback on failed boot.
- **USB alternative (required):** every release also flashes the
  plain way (`esptool` / IDE) for people who want it; the image is identical.

## 11. Repo layout sketch

```
firmware/            — ESP-IDF project (single image)
  main/              — boot, election, supervisor, worker, console
  components/bus/    — ESP-NOW transport + framing
  components/vm/     — the task VM (portable C)
  components/store/  — the disk: blocks, index client, wear leveling
host/                — dev-side tools (assembler, uploader, simulator, tests)
docs/                — this document, milestones, protocol spec
```

The VM and the protocol codec compile on-host too, so most tests run without
hardware; CI builds firmware + host tests on every push.

## 12. Naming

**Decided: ESPlab.** Repo: github.com/ESPlab-labs/ESPlab.
