# Testing without boards (the emulator and the test suite)

Real hardware is the target, but you don't need any to develop or test
ESPlab. Everything here runs the **same core code** the boards run — only
the radio is different (UDP instead of ESP-NOW).

## The test suite (fastest check)

```bash
make test
```

A deterministic cluster simulator that exercises the machine's brain:
coordinator election, work sharing, worker death, brain death and
re-election, pooled storage, VM jobs, sandbox kill, and message
fragmentation. Must print `ALL TESTS PASSED`. CI runs it on every push,
alongside the real ESP32 firmware build.

## Virtual boards (a live cluster on your computer)

`host/vboard.c` is one ESPlab node as a Linux process. Three of them
behave exactly like three real boards:

```bash
make
./build/vboard --id 1 --cores 2 --ram 290 --flash 4 --console &
./build/vboard --id 2 --chip s3 --cores 2 --ram 512 --flash 8 --console &
./build/vboard --id 3 --cores 2 --ram 290 --flash 4 --console &
```

They announce, elect (the S3 wins on merit), and hot-join — then any
board's `(esplab)` console drives the machine:

```text
submit search 0 10000 13      # pooled search across all boards
jobs                          # every job, from any board
put greeting hello
get greeting                  # write on one board, read anywhere
```

The prebuilt `vboard` binary ships in
[Releases](https://github.com/ESPlab-labs/ESPlab/releases) if you'd rather
not compile.

### Isolating test clusters

Virtual clusters find each other over UDP on a group port (default 7777).
Run a second, independent cluster with `ESPLAB_VB_PORT=7900 ./build/vboard
...` — useful for testing against a live setup without disturbing it.

## What the emulator is *not*

It is not a timing model. Band compute on vboard is deliberately throttled
and scheduling includes polling sleeps, so **throughput numbers from
virtual boards mean nothing** — correctness only. Every byte-exact proof
(such as ESP On Demand's 75-frame clip) was earned on vboard and transfers
to real boards because the wire protocol and VM are identical; the *speed*
story is measured on real hardware only (see
[docs/HARDWARE.md](HARDWARE.md)).

## The full pipeline test (video)

ESP On Demand's walkthrough has an emulator path that assembles the whole
chain — virtual boards → WebUI → media service → browser player:
[ESP-On-Demand QUICKSTART](https://github.com/ESPlab-labs/ESP-On-Demand#readme).
