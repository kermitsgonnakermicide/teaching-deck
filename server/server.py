#!/usr/bin/env python3
"""
Teaching Deck - Test Program
============================
The test program the user asked for:
  - creates a WiFi access point            (--ap)
  - reads the PSP teaching-deck commands   (UDP :9090)
  - can relay those commands to the robot  (--forward-to)
  - can receive the ROBOT's camera streams (--video-sink) to prove they arrive

Usage:
    python3 server.py                                       # read commands only
    python3 server.py --ap --ssid RobotNet                  # + create WiFi AP
    python3 server.py --forward-to 127.0.0.1:9092           # + relay cmds to robot agent
    python3 server.py --video-sink                          # + receive robot camera streams
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass

# --------------------------------------------------------------------------
# Button bitmask values (must match psp-app/input.h PSP_CTRL_*)
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
# Robot camera stream protocol (robot -> sink), one frame per UDP datagram:
#   magic 0xED 0x01 | type u8 (1=JPEG,2=RGB565) | cam u8 | w u8 | h u8 | size u16 | payload
# --------------------------------------------------------------------------
STREAM_MAGIC = b"\xED\x01"
FRAME_JPEG = 1
FRAME_RGB565 = 2
STREAM_HEADER = struct.Struct("<2sBBBBH")


@dataclass
class ButtonState:
    buttons: int = 0
    analog_x: int = 0
    analog_y: int = 0
    timestamp: int = 0
    last_rx: float = 0.0
    pkt_count: int = 0
    src_ip: str = ""


@dataclass
class StreamStats:
    frames: int = 0
    by_cam: dict = None
    bytes: int = 0
    last_meta: str = ""

    def __post_init__(self):
        if self.by_cam is None:
            self.by_cam = {}


def decode_button_packet(data: bytes) -> ButtonState:
    """ButtonPacket: buttons(I) analog_x(i) analog_y(i) timestamp(I) -> 16 bytes."""
    s = ButtonState()
    if len(data) < 16:
        return s
    s.buttons, s.analog_x, s.analog_y, s.timestamp = struct.unpack("<IiiI", data[:16])
    return s


def buttons_to_names(mask: int) -> str:
    return " + ".join(name for b, name in BUTTON_NAMES if mask & b) or "none"


def parse_stream_frame(data: bytes):
    """Return (cam_id, w, h, ftype, payload) or None if it's not a camera frame."""
    if len(data) < STREAM_HEADER.size or data[:2] != STREAM_MAGIC:
        return None
    _, ftype, cam, w, h, size = STREAM_HEADER.unpack(data[:STREAM_HEADER.size])
    payload = data[STREAM_HEADER.size:]
    if size <= len(payload):
        payload = payload[:size]
    return cam, w, h, ftype, payload


def draw_state(s: ButtonState, stats: StreamStats, now: float, last_send: float, fwd=""):
    sys.stdout.write("\033[2J\033[H")
    gap = now - s.last_rx
    print("=== TEACHING DECK - TEST PROGRAM (AP + command reader) ===")
    print(f"Deck connected : {'YES' if gap < 3.0 else 'NO (waiting...)'}   (rx {gap:.1f}s ago)")
    if s.src_ip:
        print(f"Deck IP        : {s.src_ip}   <- send this to robot_agent as its cmd source")
    print(f"Commands       : {s.pkt_count}   {(s.pkt_count / max(now - last_send, 0.001)):0.1f}/s")
    print(f"Buttons        : 0x{s.buttons:08X} -> {buttons_to_names(s.buttons)}")
    print(f"Analog         : X={s.analog_x:+4d}  Y={s.analog_y:+4d}  (t={s.timestamp})")
    hat_x = "L" if s.analog_x < -40 else ("R" if s.analog_x > 40 else "-")
    hat_y = "F" if s.analog_y < -40 else ("B" if s.analog_y > 40 else "-")
    print(f"Movement       : {hat_y}{hat_x}")
    if fwd:
        print(f"Relay          : -> {fwd}")
    print()
    print(f"=== ROBOT CAMERA STREAMS (sink) ===")
    print(f"Frames rx      : {stats.frames}   ({stats.bytes/1024.0:.0f} KiB)")
    for cam, n in sorted(stats.by_cam.items()):
        print(f"  cam {cam}      : {n} frames")
    if stats.last_meta:
        print(f"  last          : {stats.last_meta}")
    print()
    print("Quit with Ctrl+C")


def create_ap(ssid: str, password: str = "", interface: str = ""):
    """Create an access point using nmcli (NetworkManager)."""
    if not shutil.which("nmcli"):
        print("ERROR: nmcli not found. Install NetworkManager or use iw directly.")
        return False

    if not interface:
        out = subprocess.run(["nmcli", "-t", "-f", "DEVICE,TYPE", "device"],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            dev, typ = line.split(":")
            if typ == "wifi":
                interface = dev
                break
    if not interface:
        print("ERROR: no WiFi interface found")
        return False

    print(f"[AP] Creating hotspot '{ssid}' on {interface}...")
    cmd = ["nmcli", "device", "wifi", "hotspot", "ifname", interface,
           "ssid", ssid or "RobotNet"]
    if password and len(password) >= 8:
        cmd += ["password", password]
    try:
        subprocess.run(cmd, check=True)
        print(f"[AP] Success. SSID: {ssid or 'RobotNet'}")
        print("[AP] IP 10.42.0.1/24 on that interface - set SERVER_IP in psp-app/network.h")
        return True
    except subprocess.CalledProcessError as e:
        print(f"[AP] Failed: {e}")
        return False


def web_dashboard(port: int, get_state, get_stats):
    import threading
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            s = get_state()
            st = get_stats()
            now = time.time()
            gap = now - s.last_rx
            cams = ", ".join(f"cam{c}:{n}" for c, n in sorted(st.by_cam.items()))
            body = (
                "<html><head><title>Teaching Deck</title>"
                "<meta http-equiv='refresh' content='1'></head><body>"
                "<h1>Teaching Deck</h1>"
                f"<p>Deck: {'YES' if gap < 3 else 'NO'} ({gap:.1f}s ago) "
                f"IP {s.src_ip}</p>"
                f"<p>Commands: {s.pkt_count}</p>"
                f"<p><b>Buttons:</b> {buttons_to_names(s.buttons)}</p>"
                f"<p><b>Analog:</b> X={s.analog_x}  Y={s.analog_y}</p>"
                f"<h2>Robot streams</h2><p>Frames: {st.frames} ({int(st.bytes/1024)} KiB) "
                f"[{cams}]</p>"
                f"<p>{st.last_meta}</p>"
                "</body></html>"
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body.encode())

        def log_message(self, *a):
            pass

    srv = HTTPServer(("", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f"[WEB] Dashboard listening on http://0.0.0.0:{port}")


def main():
    ap = argparse.ArgumentParser(description="Teaching Deck test program (AP + command reader)")
    ap.add_argument("--port", type=int, default=9090, help="UDP port for PSP commands")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--ap", action="store_true", help="create WiFi AP on start")
    ap.add_argument("--ssid", default="RobotNet")
    ap.add_argument("--password", default="")
    ap.add_argument("--forward-to", default="", help="host:port to relay commands to (robot)")
    ap.add_argument("--video-sink", action="store_true", help="receive robot camera streams")
    ap.add_argument("--video-port", type=int, default=9091)
    ap.add_argument("--save-videos", default="", help="folder to save received frames")
    ap.add_argument("--web", action="store_true", help="serve web dashboard")
    ap.add_argument("--web-port", type=int, default=9092)
    args = ap.parse_args()

    if args.ap:
        create_ap(args.ssid, args.password)

    state = ButtonState()
    stats = StreamStats()
    last_send = time.time()

    # --- command socket (reads PSP) ---
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    sock.settimeout(0.2)
    print(f"[CMD] Listening for deck commands on {args.bind}:{args.port}")

    # --- command relay (to robot agent / internet) ---
    fwd = None
    fsock = None
    if args.forward_to:
        host, port = args.forward_to.rsplit(":", 1)
        fwd = (host, int(port))
        fsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        print(f"[REL] Forwarding deck commands to {args.forward_to}")

    # --- video sink (receives robot cams) ---
    vsock = None
    if args.video_sink:
        vsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        vsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        vsock.bind((args.bind, args.video_port))
        vsock.settimeout(0.2)
        print(f"[VID] Receiving robot camera streams on {args.bind}:{args.video_port}")
        if args.save_videos:
            os.makedirs(args.save_videos, exist_ok=True)

    if args.web:
        web_dashboard(args.web_port, lambda: state, lambda: stats)

    print("\nDecoding ButtonPacket: buttons(I) analog_x(i) analog_y(i) timestamp(I) [16B]\n")
    draw_state(state, stats, time.time(), last_send, args.forward_to)

    try:
        last_draw = 0.0
        video_shots = {}
        while True:
            # poll command socket
            try:
                data, addr = sock.recvfrom(2048)
                if len(data) == 16:
                    state = decode_button_packet(data)
                    state.last_rx = time.time()
                    state.pkt_count += 1
                    if not state.src_ip:
                        state.src_ip = addr[0]
                        print(f"\n[CMD] deck is at {addr[0]}:{addr[1]} - set robot_agent "
                              f"--cmd-source / forward accordingly\n")
                    if fwd:
                        fsock.sendto(data, fwd)
            except socket.timeout:
                pass

            # poll video sink
            if vsock:
                try:
                    vdata, _ = vsock.recvfrom(65536)
                    parsed = parse_stream_frame(vdata)
                    now = time.time()
                    if parsed:
                        cam, w, h, ftype, payload = parsed
                        stats.frames += 1
                        stats.bytes += len(vdata)
                        stats.by_cam[cam] = stats.by_cam.get(cam, 0) + 1
                        stats.last_meta = f"cam{cam} {w}x{h} {'JPEG' if ftype==FRAME_JPEG else 'RGB565'} " \
                                          f"{len(payload)}B ({now:.0f}s)"
                        if args.save_videos and (cam not in video_shots or
                                                 time.time() - video_shots.get(cam, 0) > 1.0):
                            video_shots[cam] = now
                            ext = "jpg" if ftype == FRAME_JPEG else "bin"
                            with open(os.path.join(args.save_videos,
                                                   f"cam{cam}_{int(now)}.{ext}"), "wb") as f:
                                f.write(vdata)
                except socket.timeout:
                    pass

            if time.time() - last_draw > 0.25:
                draw_state(state, stats, time.time(), last_send, args.forward_to)
                last_draw = time.time()
    except KeyboardInterrupt:
        print("\n[BYE]")
    finally:
        sock.close()
        if vsock:
            vsock.close()


if __name__ == "__main__":
    main()