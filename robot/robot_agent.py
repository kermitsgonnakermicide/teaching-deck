#!/usr/bin/env python3
"""
Robot Agent - the executable that lives ON THE ROBOT.

This is the part that:
  1. connects to the network / internet
  2. streams N low-quality, laggy camera feeds (UDP)
  3. receives the button/analog "teaching deck" commands sent by the PSP
  4. maps them onto simulated robot outputs (velocities, lights, beeps)

The cameras are the ROBOT's cameras - the PSP (teaching deck) only sends
controls; it never captures video.

Modes:
  --sim        use synthetic camera feeds (default; switch to real capture hooks)
  --receive    also bind a UDP socket and decode the PSP button stream

Example (test on one machine):
  # terminal 1: the AP + command reader (test program)
  sudo python3 ../server/server.py --ap --forward-to 127.0.0.1:9092

  # terminal 2: the robot
  python3 robot_agent.py --receive --cmd-port 9092 --sink 127.0.0.1:9091

  # PSP connects to the AP and points at <AP-host>:9090
"""

import argparse
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass

# --------------------------------------------------------------------------
# Command protocol (from PSP teaching deck) - matches psp-app/input.h
# --------------------------------------------------------------------------
BTN_SELECT = 0x1
BTN_START = 0x8
BTN_UP = 0x10
BTN_RIGHT = 0x20
BTN_DOWN = 0x40
BTN_LEFT = 0x80
BTN_L = 0x100
BTN_R = 0x200
BTN_TRIANGLE = 0x1000
BTN_CIRCLE = 0x2000
BTN_CROSS = 0x4000
BTN_SQUARE = 0x8000
BTN_NOTE = 0x800

BUTTON_NAMES = [
    (BTN_SELECT, "SELECT"), (BTN_START, "START"), (BTN_UP, "UP"),
    (BTN_RIGHT, "RIGHT"), (BTN_DOWN, "DOWN"), (BTN_LEFT, "LEFT"),
    (BTN_L, "L"), (BTN_R, "R"), (BTN_TRIANGLE, "TRIANGLE"),
    (BTN_CIRCLE, "CIRCLE"), (BTN_CROSS, "CROSS"), (BTN_SQUARE, "SQUARE"),
    (BTN_NOTE, "NOTE"),
]

# --------------------------------------------------------------------------
# Camera stream protocol (robot -> sink): one frame per UDP datagram
#   magic 0xED 0x01 | type u8 (1=JPEG, 2=RGB565) | cam u8 | w u8 | h u8 | size u16 | payload
# --------------------------------------------------------------------------
STREAM_MAGIC = b"\xED\x01"
FRAME_JPEG = 1
FRAME_RGB565 = 2


@dataclass
class Command:
    buttons: int = 0
    analog_x: int = 0
    analog_y: int = 0
    timestamp: int = 0
    pkt_count: int = 0
    last_rx: float = 0.0


def decode_command(data: bytes) -> Command:
    c = Command()
    if len(data) < 16:
        return c
    c.buttons, c.analog_x, c.analog_y, c.timestamp = struct.unpack("<IiiI", data[:16])
    return c


def buttons_to_names(mask: int) -> str:
    return " + ".join(name for b, name in BUTTON_NAMES if mask & b) or "none"


# --------------------------------------------------------------------------
# Synthetic "robot" - emits two camera feeds + reacts to commands
# --------------------------------------------------------------------------
class Robot:
    def __init__(self, cameras=2, width=160, height=120, fps=8, jpeg_quality=30,
                 fmt="rgb565"):
        self.cameras = cameras
        self.w, self.h = width, height
        self.fps = fps
        self.q = jpeg_quality
        self.fmt = fmt
        self.t = 0

    def _frame(self, cam_id, t):
        """Return pixel data as (w, h, mode, bytes) for one synthetic cam."""
        # Cheap procedural pattern seeded by camera id + time so you can
        # verify both streams are distinct and "moving" even at 1 byte/frame res.
        w, h = self.w, self.h
        if cam_id == 0:
            # left camera: horizontal sweep + noise
            band = (t // 2) % w
            rows = [
                bytes((0xFF if x % 47 == 0 or x == band else (x * t) & 0xFF)
                      for x in range(w))
                for _ in range(h)
            ]
        else:
            # right camera: "shadow gradient" (fake depth)
            rows = [
                bytes((((y * 3) + x + (t * 5)) & 0xFF) & (0xF0 if (x + t) % 8 else 0xFF)
                      for x in range(w))
                for y in range(h)
            ]
        return b"".join(rows), "gray"

    def make_frame(self, cam_id, t):
        gray, _ = self._frame(cam_id, t)

        if self.fmt == "rgb565":
            # Raw RGB565 (BGR565 little-endian). This is what the PSP can blit
            # straight into its framebuffer - no codec needed on the deck.
            rgb = bytearray(self.w * self.h * 2)
            o = 0
            for b in gray:
                v = b >> 3              # 0..31
                p = ((v & 0x1F) << 11) | ((v & 0x3F) << 5) | (v & 0x1F)  # r5 g6 b5, gray
                rgb[o] = p & 0xFF
                rgb[o + 1] = (p >> 8) & 0xFF
                o += 2
            return FRAME_RGB565, bytes(rgb)

        # JPEG (MJPG-style) for browser/mjpeg sinks that can decode it.
        try:
            from PIL import Image
            img = Image.new("L", (self.w, self.h))
            img.putdata(list(gray))
            import io
            buf = io.BytesIO()
            img.save(buf, format="JPEG", quality=self.q, optimize=False)
            return FRAME_JPEG, buf.getvalue()
        except ImportError:
            return self.make_frame(cam_id, t)


def build_stream_packet(ftype, cam_id, w, h, payload):
    header = STREAM_MAGIC + struct.pack("<BBBBH", ftype, cam_id, w, h, len(payload))
    return header + payload


class Streamer:
    """Sends the robot's camera feeds to a sink over UDP."""

    def __init__(self, sink_host, sink_port, robot, stop_event):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.dst = (sink_host, sink_port)
        self.robot = robot
        self.stop = stop_event
        self.sent = 0

    def run(self):
        while not self.stop.is_set():
            t = int(time.time() * self.robot.fps) % (1 << 20)
            self.robot.t = t
            for cam in range(self.robot.cameras):
                ftype, payload = self.robot.make_frame(cam, t + cam * self.robot.w)
                try:
                    self.sock.sendto(
                        build_stream_packet(ftype, cam, self.robot.w, self.robot.h, payload),
                        self.dst,
                    )
                    self.sent += 1
                except OSError:
                    pass
            time.sleep(1.0 / self.robot.fps)


# --------------------------------------------------------------------------
# Command receiver - the robot listens for PSP button stream
# --------------------------------------------------------------------------
class CommandReceiver(threading.Thread):
    def __init__(self, port, stop_event, log_event):
        super().__init__(daemon=True)
        self.port = port
        self.stop = stop_event
        self.log = log_event
        self.cmd = Command()

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", self.port))
        sock.settimeout(0.2)
        while not self.stop.is_set():
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            if len(data) == 16:
                self.cmd = decode_command(data)
                self.cmd.pkt_count += 1
                self.cmd.last_rx = time.time()
                self.log.set()
        sock.close()


def print_robot_state(cmd, step, motor_l, motor_r):
    sys.stdout.write("\033[2J\033[H")
    now = time.time()
    gap = now - cmd.last_rx
    print("=== ROBOT AGENT - camera + command receiver ===")
    print(f"Deck connected : {'YES' if gap < 3.0 else 'NO'}   (rx {gap:.1f}s ago)")
    print(f"Commands       : {cmd.pkt_count}")
    print(f"Buttons        : 0x{cmd.buttons:08X} -> {buttons_to_names(cmd.buttons)}")
    print(f"Analog         : X={cmd.analog_x:+4d} Y={cmd.analog_y:+4d}  (t={cmd.timestamp})")
    print(f"Motors         : L={motor_l:+6.1f}  R={motor_r:+6.1f}  (units/s)")
    print(f"Streaming      : {step.sent} frames sent   [sim mode]")
    print()
    print("Press Ctrl+C to stop")


def drive_model(cmd):
    """Crude drive kinematics: tank drive from analog stick + buttons."""
    y, x = cmd.analog_y, cmd.analog_x
    spd = min(abs(y) / 127.0, 1.0) * (1 if y < 0 else -1)  # up=forward (PSP stick)
    turn = x / 127.0
    l = (spd + turn) * 100
    r = (spd - turn) * 100
    return l, r


# Telemetry packet sent back to the lab: buttons(I) ax(i) ay(i) motorL(i) motorR(i) ts(I)
TELEMETRY_FMT = "<IiiiiI"


def build_telemetry(cmd, l, r):
    return struct.pack(TELEMETRY_FMT, cmd.buttons, cmd.analog_x, cmd.analog_y,
                       int(l), int(r), int(time.time() * 1000) & 0xFFFFFFFF)


def main():
    ap = argparse.ArgumentParser(description="Robot agent - cameras + PSP commands")
    ap.add_argument("--sink", default="127.0.0.1", help="host that receives camera streams")
    ap.add_argument("--sink-port", type=int, default=9091)
    ap.add_argument("--fps", type=int, default=8)
    ap.add_argument("--quality", type=int, default=30, help="JPEG quality (5-40 = low/laggy)")
    ap.add_argument("--format", choices=["rgb565", "jpeg"], default="rgb565",
                    help="frame payload format (rgb565 = PSP-native)")
    ap.add_argument("--cameras", type=int, default=2)
    ap.add_argument("--width", type=int, default=160)
    ap.add_argument("--height", type=int, default=120)
    ap.add_argument("--receive", action="store_true",
                    help="bind UDP and decode the PSP button stream")
    ap.add_argument("--cmd-port", type=int, default=9090)
    ap.add_argument("--forward-cmds-to", default="",
                    help="optional host:port to forward parsed commands to (e.g. internet relay)")
    ap.add_argument("--telemetry-to", default="",
                    help="optional host:port to send motor/status telemetry to (lab UI)")
    args = ap.parse_args()

    stop = threading.Event()
    log = threading.Event()
    robot = Robot(args.cameras, args.width, args.height, args.fps, args.quality,
                  args.format)
    streamer = Streamer(args.sink, args.sink_port, robot, stop)

    print(f"[ROBOT] streaming {args.cameras} x {args.width}x{args.height} @ {args.fps}fps "
          f"(q={args.quality}) to {args.sink}:{args.sink_port}")

    # Lightweight command-byte forwarding (like a teleop gateway over the internet)
    if args.forward_cmds_to:
        host, port = args.forward_cmds_to.rsplit(":", 1)
        fwd = (host, int(port))
        fsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    else:
        fwd, fsock = None, None

    if args.telemetry_to:
        thost, tport = args.telemetry_to.rsplit(":", 1)
        tele = (thost, int(tport))
        tsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        print(f"[ROBOT] sending telemetry to {args.telemetry_to}")
    else:
        tele, tsock = None, None

    cmd = Command()
    recv = None
    if args.receive:
        recv = CommandReceiver(args.cmd_port, stop, log)
        recv.start()
        print(f"[ROBOT] listening for deck commands on UDP {args.cmd_port}")

    spin = threading.Thread(target=streamer.run, daemon=True)
    spin.start()

    try:
        last_draw = time.time()
        while not stop.is_set():
            if log.is_set():
                log.clear()
                if recv:
                    cmd = recv.cmd
                if fwd:
                    fsock.sendto(struct.pack("<IiiI", cmd.buttons, cmd.analog_x,
                                             cmd.analog_y, cmd.timestamp), fwd)
            if time.time() - last_draw >= 1.5:
                last_draw = time.time()
                l, r = drive_model(cmd)
                if tsock:
                    tsock.sendto(build_telemetry(cmd, l, r), tele)
                print_robot_state(cmd, streamer, l, r)
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        spin.join(timeout=2)
        print("\n[ROBOT] stopped")


if __name__ == "__main__":
    main()