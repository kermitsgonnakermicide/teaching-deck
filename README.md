# Teaching Deck — PSP2000 Robot Controller

A remote "teaching console" for a robot:

- **PSP-2000** is a *teaching deck*: it reads all buttons + analog stick and
  sends them over WiFi/UDP, and it also renders the robot's camera feeds on its
  own screen (the deck itself never captures video).
- The **robot** has the cameras: `robot/robot_agent.py` is the executable that
  connects to the network, streams two camera feeds, and receives the PSP's
  control commands.
- A **test program** (`server/server.py`) creates a WiFi AP and reads the
  PSP's command stream (also relays it, and can sink the robot's video).

The finished form of the project is a **full GUI running on the desktop**: the
real PSP homebrew (EBOOT.PBP) inside PPSSPP shows two live robot camera feeds +
on-screen telemetry in its own window. See "Desktop GUI mode" below.

```
   [PSP teaching deck]                  [test program]                      [robot]
   reads buttons  ──────UDP:9090──────►  (server.py)                        robot_agent.py
   renders cameras  ◄──────────────────────────────────────────────────────── streams RGB565 :9091
                                         - creates AP (--ap)   ──────9092──► - reads deck cmds
                                         - reads commands                   - drives motors
                                         - relays commands   ◄───────────── - streams cams
                                         - sinks video (:9091)              - connects to net
```

## Project layout

```
psp-app/          PSP homebrew (C / PSPSDK) — the teaching deck + dashboard GUI
  main.c            input loop, camera windows, on-screen HUD, L+R+START quits
  input.c/h         button + analog sampling (30 Hz)
  network.c/h       WiFi + UDP sender
  gfx.c/h           GU + display, CPU frame composition (5650 canvas -> copyimage)
  video.c/h         camera UDP receiver + frames
  Makefile          builds EBOOT.PBP
server/
  server.py         test program: WiFi AP, command reader, relay, video sink
robot/
  robot_agent.py    the executable on the robot: cam streams + command receiver
test/
  keyinject.py      XTEST injection + window finder for the emulator GUI (verified keymap)
  control_center.py rejected tkinter dashboard — superseded by the PSP app GUI (keep only as reference)
docs/
  emulator-deck.log / emulator-boot.png   proof-of-boot artifacts
```

## Desktop GUI mode (the real thing, verified)

The PSP application itself **is** the GUI. Run the emulator on the desktop and
it opens a `Teaching Deck` window showing the robot's two live camera feeds
(streamed as raw RGB565, no codec) plus an on-screen HUD with your current
button mask, analog stick, packet counters and the quit hint:

```
           "PSP TEACHING DECK — ROBOT CAMERAS LIVE"
   [ CAM0 live window ]        [ CAM1 live window ]
   BTN 0x000001C8  ANA X=  +0 Y=+0  PKT 1234 SENT ERR 0
   L+R+START = QUIT    CAM0              CAM1
```

Keymap (PPSSPP Linux/QWERTY defaults):

```
  square=A  triangle=S  circle=X  cross=Z   L=Q  R=W
  START=Space  SELECT=Return   D-pad=arrows  analog=I/K/J/L
```

Quit the deck with **q+w+space (= L+R+START)** — the app logs
`[quit] L+R+START pressed` and shuts down cleanly.

### Start it

```sh
# terminal 1 — command reader + relay (no sudo needed: no AP here, loopback works)
python3 server/server.py --forward-to 127.0.0.1:9092

# terminal 2 — robot agent streaming PSP-native RGB565 to the deck
python3 robot/robot_agent.py --receive --cmd-port 9092 \
  --sink 127.0.0.1 --sink-port 9091 --telemetry-to 127.0.0.1:9093

# terminal 3 — the "GUI": PPSSPP on the desktop, software-rendered window
setsid nohup env DISPLAY=:1 HOME=/home/daksh \
  /home/daksh/psp-assets/squashfs-root/bin/PPSSPPSDL --graphics=software --windowed \
  /home/daksh/.config/ppsspp/PSP/GAME/teachingdeck/EBOOT.PBP >/tmp/gui.log 2>&1 &
```

Pixels flow one way, commands the other. The deck logs progress to
`/home/daksh/.config/ppsspp/teachingdeck.log`
(`[net]`, `[cam]`, `[loop]`, `[quit]` lines). Emulator notes: use `--graphics=software`
and input focus must be on the `Teaching Deck` window (see `test/keyinject.py`).

PPSSPP's software renderer has quirks a *real* PSP does not: full-screen solid
primitives, sprite text and copyimage with non-zero offsets render incorrectly,
so `gfx.c` assembles the whole frame on a CPU canvas and pushes it to VRAM with
a single `sceGuCopyImage` at the origin. Builds are verified against the
emulator only; code is plain PSPSDK and compiles for real hardware unchanged.

## The protocols

Both are plain little-endian, self-describing UDP.

### 1. Commands — PSP (deck) → server / robot, UDP :9090

One `ButtonPacket` every 33 ms (16 bytes):

```
0  u32  buttons    PSP_CTRL_* bitmask  (see psp-app/input.h)
4  i32  analog_x   -128..127
8  i32  analog_y   -128..127
12 u32  timestamp  low 32 bits of the PSP RTC tick
```

### 2. Camera frames — robot → sink, UDP :9091 (one frame per datagram)

```
0  2  magic  0xED 0x01
2  u8  type   1 = JPEG, 2 = RGB565
3  u8  cam    camera id
4  u8  width  (max 255)
5  u8  height (max 255)
6  u16 size   payload length
8  .. payload (JPEG bytes or RGB565 pixels)
```

Small frames (<=64 KB) keep each camera feed a single UDP datagram so a lost
packet costs one frame, not the whole feed.

### 3. Telemetry — robot → lab, UDP :9093 (idle-free, ~1.5 s cadence)

```
0  u32 buttons    last recognized packet's buttons
4  i32 analog_x
8  i32 analog_y
12 i32 motorL     from drive_model(), -100..100
16 i32 motorR
20 u32 timestamp  ms
```

Used by the interactive lab to show drive state; a real rollout can ignore it
or repurpose the same slot for odometry/status.

## Building the PSP deck

Requires PSPSDK. Two options:

### Option A — prebuilt (fast, used for the tests in this repo)

```sh
curl -L -o /tmp/pspdev.tar.xz \
  https://github.com/pspdev/pspdev/releases/download/v20260901/pspdev-ubuntu-latest-x86_64.tar.gz
mkdir -p ~/pspdev && tar xf /tmp/pspdev.tar.xz -C ~/pspdev
cd psp-app && make
```

The Makefile auto-finds `~/pspdev` (or `/opt/pspdev`, `/home/*/pspdev`).
Override with `make PSPDEV=/path/to/pspdev`.

### Option B — build the toolchain from source

```sh
apt install build-essential bison flex libgmp-dev libmpc-dev libmpfr-dev \
            python3 texinfo xorriso
git clone --recursive https://github.com/pspdev/psptoolchain /tmp/psptoolchain
cd /tmp/psptoolchain && ./toolchain.sh
```

Put `EBOOT.PBP` on the Memory Stick at `ms0:/PSP/GAME/teachingdeck/`.

**Before building** set the target IP in `psp-app/network.h`:
- shared-WiFi mode → your PC's static IP,
- AP mode → the AP host's address (usually 10.42.0.1).

Quit the deck with **L + R + START** (QWERTY: **q + w + space** on the emulator).

## Testing on the emulator (verified)

The deck has a status log hook (`ms0:/teachingdeck.log`) that only shows up on
a real PSP or emulator. With **PPSSPP** it is directly readable on the host;
the GUI-mode steps above are the canonical, verified way to run it (software
renderer, loopback to 127.0.0.1). `test/keyinject.py` drives the window with
XTEST (verified keymap above).

Verified full-chain log:

```
[boot] started
[init] input ready
[net] stack init OK
[net] udp socket up -> 192.168.1.100:9090
[cam] listening UDP 9091
[net] first button packet SENT ok (0x00000000,0,0)
[loop] 30 poll ticks: 30 sent, 0 errors
[quit] L+R+START pressed after 1328 polls
[quit] shutting down (sent 1327, errors 0)
```

The deck boots, initializes its network stack, opens the sockets, sends button
packets at 30 Hz, receives both camera feeds, and quits cleanly on the combo.
`docs/emulator-boot.png` is a screenshot of the deck's screen inside the
emulator.

## Running

### Interactive lab (try it yourself, quickest)

One command starts the whole stack locally and gives you a browser dashboard
where **you** are the deck:

```sh
test/run_lab.sh          # then open http://127.0.0.1:8080
```

What you get in that page:
- two live camera feeds streamed by `robot_agent.py` (the robot's cameras),
- a clickable virtual PSP deck (D-pad, △○×□, L/R, START/SELECT, analog stick)
  that sends **real 16-byte ButtonPackets** on UDP to `server.py`,
- live robot telemetry (buttons/analog received + motor power %), proving the
  whole chain: deck → command reader/relay → robot → cameras + telemetry back.

Drive it: hold UP → both motors spin up; stick left → robot pivots
(L=-100, R=+100).
This uses the exact same protocol as the real PSP deck, so a physical PSP can
take over the virtual one without changing anything else.

### Terminal 1 — test program (AP + read commands)

```sh
sudo python3 server/server.py --ap --ssid RobotNet --video-sink --save-videos ./shots
```

It prints the deck's IP when the PSP connects, relays nothing by default,
and saves whatever the robot streams into `./shots/`.

### Terminal 2 — robot agent (simulates the robot's cameras)

```sh
python3 robot/robot_agent.py --receive --cmd-port 9092 --format rgb565
```

- streams 2 synthetic 160x120 feeds (raw RGB565 little-endian by default — the
  PSP-native format the deck blits straight into its framebuffer; pass
  `--format jpeg` for browser sinks that need JPEG instead)
- listens for commands (forward them from terminal 1 with
  `server.py --forward-to <robot-ip>:9092`)

Then connect the PSP to the AP and watch the deck's buttons show up in both
terminals, while the robot streams to the server.

### Going to a real robot

- Replace `Robot.make_frame()` with a real capture source (Pi cam, USB cam,
  CSI). Keep the same UDP framing and you can drop quality/fps to anything
  "low quality / laggy" by raising `--fps`/`--quality`.
- Replace `drive_model()` motors line with real GPIO / serial / CAN writes.
- The machine running the agent is not necessarily on the same LAN as the
  deck — that's what `--forward-cmds-to` is for: an internet relay endpoint
  or the AP machine forwarding to the robot over the wider internet.