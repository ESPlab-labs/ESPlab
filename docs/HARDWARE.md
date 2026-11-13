# Flashing real boards (hardware guide)

Everything you need to put ESPlab on actual ESP32 boards and see the machine
form on your desk. No prior ESP32 experience assumed.

## What you need

- One or more ESP32 boards. Any mix of these works: **ESP32** (classic),
  **ESP32-S2**, **ESP32-S3**, **ESP32-C3**. Dev boards with USB
  micro-B/USB-C are the easiest.
- A **USB data cable** per board (many cheap cables are charge-only — if
  your computer never sees a new serial port, try another cable).
- A computer with **USB ports** and about 2 GB free disk (for the build
  tools).
- That's it. No router, no Wi-Fi network, no internet at run time.

## Step 1 — install the build tools (ESP-IDF)

ESPlab firmware is built with Espressif's official toolchain, ESP-IDF v5.2
(or newer 5.x). One-time setup:

```bash
git clone --recurse-submodules -b v5.2.2 \
    https://github.com/espressif/esp-idf.git ~/esp-idf
~/esp-idf/install.sh            # Windows: run install.bat instead
. ~/esp-idf/export.sh           # do this in every new terminal
```

Verify: `idf.py --version` prints a version.

## Step 2 — build the firmware

From the ESPlab repo:

```bash
cd firmware
idf.py set-target esp32         # match YOUR board (see table below)
idf.py build
```

| your board says | set-target |
|---|---|
| ESP32 / ESP32-WROOM-32 | `esp32` |
| ESP32-S2 | `esp32s2` |
| ESP32-S3 | `esp32s3` |
| ESP32-C3 | `esp32c3` |

One firmware image for every board — roles (coordinator/worker) are elected
at run time, never flashed. If you flash a mix of chips, build once per chip
type into separate copies of the repo (or just re-run `set-target` + build).

## Step 3 — flash each board

Plug the first board in and find its serial port:

- Linux: `/dev/ttyUSB0` or `/dev/ttyACM0` (check `ls /dev/ttyUSB* /dev/ttyACM*`)
- macOS: `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`
- Windows: `COM3`, `COM4`, … (Device Manager → Ports)

Then:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

You'll see the machine's own log. It should look like:

```
ESPlab node up: aabbccddeeff
election gen=1 (no cluster heard)
I am the coordinator
```

The first board elects itself coordinator. Now flash the rest — same
command, one board at a time (each on its own port). When a second board
boots it prints instead:

```
election gen=2 ... I am a worker (coordinator seen)
joined cluster: 2 nodes
```

Type `Ctrl-]` to leave the monitor.

## Step 4 — use the machine from the serial console

Re-open any board's console with `idf.py -p PORT monitor` (no flash). You
get the `(esplab)` prompt — every board has the full console, worker or
coordinator:

```
(esplab) status
(esplab) submit search 0 10000 13
(esplab) jobs
(esplab) put greeting hello from the machine
(esplab) get greeting
```

`status` should list every board with its chip type. That's the machine
working on your desk.

## Step 5 — the dashboard and video

From the computer the boards are plugged into (or any machine that can reach
them over USB — the web services run where the serial ports are):

```bash
python3 tools/webui/webui.py                    # dashboard: http://localhost:8080
python3 tools/eod_serve.py --webui http://127.0.0.1:8080 \
        --clip clip.eod --port 8090             # video:    http://localhost:8090
```

Real boards + dashboard + video = the full end-to-end. The web services
speak the boards' serial console over USB; nothing else is needed.

## Troubleshooting

| symptom | fix |
|---|---|
| no serial port appears | charge-only cable; try a data cable. On 5 V boards check the power switch/onboard LED. |
| `Failed to connect` while flashing | hold the board's **BOOT** button while it resets, then retry |
| boards never join (`no cluster heard` forever) | keep boards within a meter for the first test; a metal case or desk between boards blocks the radio |
| one board elected, second still says no cluster | wait ~10 s; join attempts repeat automatically |
| `idf.py` not found | you skipped `. ~/esp-idf/export.sh` in this terminal |
| dashboard shows 0 boards | the web service must run on the machine with the USB serial ports; check `tools/webui/README.md` |

## Power notes

One USB port per board is fine (each board draws < 0.5 A). A powered USB
hub is the tidy way to run 5+ boards. Boards do not need to be networked to
anything else.
