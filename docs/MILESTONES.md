# ESP32 Cluster — Build order (milestones)

Each milestone ends with something you can *see working* on the real boards.
Distances-to-goal are stated plainly.

## M0 — Repo and toolchain (the workbench)
- Create GitHub repo (name decided), MIT/Apache-2.0 license, README with the
  one-paragraph vision.
- ESP-IDF builds a do-nothing firmware for classic ESP32 on this machine (or
  via CI if the Pi toolchain is too slow — decide by trying).
- CI: builds firmware + host tests on every push.
- **You'll see:** `idf.py build` (or CI) producing a flashable image, and a
  repo with a README that isn't embarassing.

## M1 — Hello, cluster (boards find each other)
- ESP-NOW bus layer with framing + chunking, encrypted with the shared key.
- Boot behavior: boards discover each other, elect a coordinator, hot-join.
- Capability cards exchanged. Heartbeats + silence detection.
- Serial console on the coordinator: `status`, `nodes`, `log`.
- **You'll see:** plug in all 3, type `nodes`, and all three boards appear
  with their chip info. Unplug one, it drops off the list; plug it back, it
  rejoins in seconds. This is the moment the project becomes real.

## M2 — The machine does work (job engine)
- Job spec over serial (`submit type=search range=...`), supervisor
  auto-chunks by capability card, pull scheduling, results assembled.
- Dead-core rescheduling: kill a board mid-job, the job finishes anyway.
- Built-in demo jobs: `hello` (every core reports in) and a parallel
  brute-force search.
- **You'll see:** the search job finish in roughly a third of the time one
  board takes, and — the better demo — yank a board's power mid-job and the
  job still completes with a line in the log about the rescheduled chunk.

## M3 — The task VM (users' code runs on the machine)
- VM core (portable C), ~32 opcodes, sandbox slots, timeslice kill.
- Host tools: assembler + simulator; upload blobs over serial; determinism
  test suite runs host-side on every push.
- One nontrivial example task shipped (e.g. distributed hash-crack or
  collatz explorer) running via VM on all boards.
- **You'll see:** a blob you assembled on the laptop running simultaneously
  on all boards, results assembled into one answer. And a deliberately
  infinite-loop task getting killed by the sandbox with the machine alive.

## M4 — The disk (pooled storage)
- Block store on spare flash, supervisor-held index, one namespace (`ls`,
  `put`, `get` over serial), wear leveling, write-rarely discipline.
- Storage backend interface + SD-card adapter stub.
- **You'll see:** `put` a file on one board's console, `ls` and `get` it from
  the machine; the blocks actually live spread across boards (`disk map`
  command shows where). Unplug a board and the machine tells you exactly
  which files went offline.

## M5 — Hardening (the release candidate)
- Pairing ceremony polished (`pair` command, first-boot flow).
- Watchdogs everywhere: task kill, board silence, radio resend budgets.
- Mixed-chip testing path documented (borrow/buy one S3 or C3 to prove the
  capability-card scheduling and VM portability claims; CI covers the rest).
- Docs: protocol spec, task-writing guide, flashing guide, troubleshooting.
- **You'll see:** a friend (or a stranger's issue) able to go from bare
  boards to running the demo jobs using only the README.

## M6 — OTA and v1.0
- Dual-bank OTA from the machine's disk, board-by-board rollover with
  rollback; USB flashing documented as the alternative path.
- Version handshake: mixed firmware versions refuse to cluster (with a clear
  console message), so an interrupted OTA can't wedge the machine.
- Tag v1.0.0, release with prebuilt images.
- **You'll see:** type `update`, walk away, come back to all boards on the
  new version — and pull power on a board mid-update without bricking
  anything.

## After v1 (the backlog, unordered)
- Web UI on the coordinator (optional flash-on layer).
- Storage mirroring setting (2 copies per block).
- Leader re-election hardening for pathological radio conditions.
- More native accelerator ops; soft-float in the VM.
- Bigger-fleet testing (10+ boards), ESP-NOW channel plan for density.
