# ESPlab — many boards, one computer

ESPlab turns a pile of ESP32 boards into a single small computer. Flash every
board with the same firmware, plug them into USB power, and they find each
other, elect a coordinator, and act as **one machine** — one console, one job
list, one filesystem — with the pooled CPU and storage of all its boards.

It is not a distributed system you have to think about. It is a computer whose
parts happen to live on several circuit boards.

- **No router, no network.** Boards talk to each other directly over ESP-NOW,
  encrypted with a shared key. The cluster never touches the internet.
- **Hot join.** Flash a board, power it, and it joins the machine in seconds.
- **One firmware.** Every board runs the same image; roles are decided at
  runtime. The coordinator works like every other board.
- **Real work.** Jobs are split across boards automatically by their measured
  capabilities. Tasks run in a sandboxed bytecode VM, so one task blob runs on
  every chip — classic ESP32, S2, S3, C3 — and a runaway task dies without
  hurting the machine.
- **One disk.** The boards' spare flash presents as a single filesystem.
- **USB or OTA.** Flash by cable, or later let the machine update itself.

## Status

Pre-alpha, under active development. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
for the design and [docs/MILESTONES.md](docs/MILESTONES.md) for the build order.

## License

MIT — see [LICENSE](LICENSE).
