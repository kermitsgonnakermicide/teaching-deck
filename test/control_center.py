#!/usr/bin/env python3
"""Teaching Deck - Control Center (desktop GUI)

One proper window on your desktop:
  * live camera streams from the robot (real UDP :9091)
  * a PSP controller pad - clicks are injected with XTEST into the REAL
    PPSSPP emulator window running the REAL homebrew EBOOT.PBP
  * the same presses are mirrored as network ButtonPackets (what a PSP on
    WiFi would send), so server.py / robot_agent.py react in real time
  * live robot telemetry + motor power bars (real UDP :9093)

Run:  python3 test/control_center.py        (on the desktop DISPLAY=:1)
"""
import os
import socket
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

from PIL import Image, ImageTk

# our XTEST injection helpers
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import keyinject as kj

CAM_PORT = 9091
TEL_PORT = 9093
CMD_PORT = 9090
SERVER_CMD = ("127.0.0.1", CMD_PORT)

STREAM_FMT = "<2sBBBBH"
TELE_FMT = "<IiiiiI"
PKT_FMT = "<IiiI"

BTN_MASK = {
    "select": 0x1, "start": 0x8, "up": 0x10, "right": 0x20, "down": 0x40,
    "left": 0x80, "L": 0x100, "R": 0x200, "square": 0x8000,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000,
}

# PSP control -> (X keysym name, label)  [PPSSPP Linux/QWERTY defaults]
LAYOUT = [
    ("up", "Up", "DPAD UP"), ("down", "Down", "DPAD DOWN"),
    ("left", "Left", "DPAD LEFT"), ("right", "Right", "DPAD RIGHT"),
    ("triangle", "s", "TRI"), ("circle", "x", "CIRC"), ("cross", "z", "CROSS"),
    ("square", "a", "SQUARE"),
    ("L", "q", "L"), ("R", "w", "R"),
    ("start", "space", "START"), ("select", "Return", "SELECT"),
]
ANALOG = [("up", "i"), ("down", "k"), ("left", "j"), ("right", "l")]

DARK = "#0f1318"
PANEL = "#161c24"
ACCENT = "#3ddc55"
TEXT = "#d7e3d7"
DIM = "#5c6b5c"
GLOW = "#e6b63c"


class Deck(object):
    """Mirrors PSP state: injects into the real deck via XTEST + mirrors a
    ButtonPacket over UDP exactly as the homebrew would on WiFi."""

    def __init__(self):
        self.mask = 0
        self.ax = 0
        self.ay = 0
        self.lock = threading.Lock()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.display = None
        self.deck_win = None
        self.cam_frames = {0: None, 1: None}
        self.telemetry = None
        self._sock = None

    def _ensure_display(self):
        if self.display is None:
            try:
                self.display = kj.get_display()
            except Exception as e:
                return print(f"[deck] no display: {e}")
        if self.deck_win is None:
            try:
                self.deck_win = kj.find_deck_window(self.display)
            except Exception:
                self.deck_win = None

    def _inject_once(self, kname, down):
        self._ensure_display()
        if not self.display or self.deck_win is None:
            return
        try:
            from Xlib import X
            from Xlib.ext import xtest
            if self.deck_win:
                try:
                    self.deck_win.set_input_focus(X.RevertToPointerRoot, X.CurrentTime)
                    self.deck_win.raise_window()
                    self.display.sync()
                except Exception:
                    pass
            kc = kj._kcode(self.display, kname)
            if kc:
                xtest.fake_input(self.display, X.KeyPress if down else X.KeyRelease, kc)
                self.display.sync()
        except Exception as e:
            print(f"[deck] key error: {e}")

    def set_button(self, name, down, inject_kname=None):
        with self.lock:
            if down:
                self.mask |= BTN_MASK[name]
            else:
                self.mask &= ~BTN_MASK[name]
            pkt = struct.pack(PKT_FMT, self.mask, self.ax, self.ay,
                              int(time.time() * 1000) & 0xFFFFFFFF)
            try:
                self.udp.sendto(pkt, SERVER_CMD)
            except Exception:
                pass
        if inject_kname:
            self._inject_once(inject_kname, down)

    def set_analog(self, ax, ay):
        with self.lock:
            self.ax = int(ax)
            self.ay = int(ay)
            pkt = struct.pack(PKT_FMT, self.mask, self.ax, self.ay,
                              int(time.time() * 1000) & 0xFFFFFFFF)
            try:
                self.udp.sendto(pkt, SERVER_CMD)
            except Exception:
                pass


class ControlCenter(tk.Tk):
    def __init__(self, deck):
        super().__init__()
        self.deck = deck
        self.title("Teaching Deck - Control Center")
        self.configure(bg=DARK)
        self.geometry("1060x720+40+40")
        self.photo = {}
        self.pressed = {}
        self._build()
        self._start_network_threads()
        self.after(60, self._update_loop)

    # ---------- UI ----------
    def _build(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background=DARK)
        style.configure("DPanel.TFrame", background=PANEL)
        style.configure("TLabel", background=DARK, foreground=TEXT,
                        font=("DejaVu Sans", 11))
        style.configure("Dim.TLabel", background=PANEL, foreground=DIM,
                        font=("DejaVu Sans", 10))
        style.configure("Bold.TLabel", background=PANEL, foreground=TEXT,
                        font=("DejaVu Sans", 14))
        style.configure("Title.TLabel", background=DARK, foreground=ACCENT,
                        font=("DejaVu Sans", 18, "bold"))
        style.configure("TProgressbar", background=ACCENT, troughcolor="#0a0d11",
                        borderwidth=0)

        title = ttk.Label(self, text="TEACHING DECK  -  CONTROL CENTER",
                          style="Title.TLabel")
        title.pack(anchor="w", padx=16, pady=(12, 4))

        hint = ttk.Label(self, text=("Real homebrew on PPSSPP  |  keys: "
                                     "SQUARE=a TRIANGLE=s CIRCLE=x CROSS=z  L=q R=w  "
                                     "START=space SELECT=Enter  D-pad=arrows  analog=I/K/J/L"),
                         style="Dim.TLabel", background=DARK)
        hint.pack(anchor="w", padx=16)

        top = ttk.Frame(self)
        top.pack(fill="both", expand=True, padx=3, pady=6)

        for cam, name in ((0, "CAM 0"), (1, "CAM 1")):
            p = ttk.Frame(top, style="DPanel.TFrame")
            p.pack(side="left", fill="both", expand=True, padx=10, pady=8)
            ttk.Label(p, text=name, style="Bold.TLabel",
                      background=PANEL).pack(anchor="w", padx=8, pady=(8, 2))
            label = tk.Label(p, bg="#05070a", width=56, height=30)
            label.pack(padx=8, pady=(0, 4))
            setattr(self, f"cam{cam}", label)

        # --- telemetry panel
        tel = ttk.Frame(self, style="DPanel.TFrame")
        tel.pack(fill="x", padx=13, pady=(0, 8))
        ttk.Label(tel, text="  ROBOT TELEMETRY", style="Bold.TLabel",
                  background=PANEL).grid(row=0, column=0, sticky="w", padx=8, pady=6)
        self.t_btn = ttk.Label(tel, text="  buttons: -", background=PANEL,
                               style="Dim.TLabel")
        self.t_btn.grid(row=0, column=1, sticky="w", padx=14)
        self.t_analog = ttk.Label(tel, text="  analog: -", background=PANEL,
                                  style="Dim.TLabel")
        self.t_analog.grid(row=0, column=2, sticky="w", padx=14)
        self.t_pkts = ttk.Label(tel, text="  packets: -", background=PANEL,
                                style="Dim.TLabel")
        self.t_pkts.grid(row=0, column=3, sticky="w", padx=14)
        self.t_cams = ttk.Label(tel, text="  cam frames: -", background=PANEL,
                                style="Dim.TLabel")
        self.t_cams.grid(row=0, column=4, sticky="w", padx=14)

        self.mL = ttk.Progressbar(tel, length=420, maximum=200)
        self.mL.grid(row=1, column=0, columnspan=4, sticky="ew", padx=14, pady=(0, 2))
        self.mR = ttk.Progressbar(tel, length=420, maximum=200)
        self.mR.grid(row=2, column=0, columnspan=4, sticky="ew", padx=14, pady=(0, 8))
        self.t_motors = ttk.Label(tel, text="  L: 0%   R: 0%", background=PANEL,
                                  style="Dim.TLabel")
        self.t_motors.grid(row=1, column=4, rowspan=2, sticky="e", padx=14)

        # --- control pad
        ctl = ttk.Frame(self, style="DPanel.TFrame")
        ctl.pack(fill="both", expand=True, padx=13, pady=(0, 12))
        ttk.Label(ctl, text="  PSP CONTROLS  (click = press, release = lift",
                  style="Bold.TLabel", background=PANEL).grid(
            row=0, column=0, columnspan=5, sticky="w", padx=8, pady=6)
        btns = []
        for i, (name, kname, label) in enumerate(LAYOUT):
            b = tk.Button(ctl, text=label, width=8, bd=1, relief="ridge",
                          bg=PANEL, fg=TEXT, activebackground=ACCENT,
                          activeforeground="#061a0c", font=("DejaVu Sans", 10, "bold"))
            b.grid(row=1 + i // 4, column=i % 4, padx=7, pady=7, sticky="n")
            b.bind("<ButtonPress-1>", lambda e, n=name, k=kname: self._down(n, k))
            b.bind("<ButtonRelease-1>", lambda e, n=name, k=kname: self._up(n, k))
            b.bind("<Leave>", lambda e, n=name, k=kname: self._up(n, k))
            btns.append(b)
        self.buttons = dict(zip([n for n, _, _ in LAYOUT], btns))

        an = tk.Frame(ctl, bg=PANEL)
        an.grid(row=1, column=4, rowspan=4, padx=20, pady=10, sticky="n")
        ttk.Label(an, text="ANALOG", style="Dim.TLabel", background=PANEL).pack()
        self.ax_lbl = tk.Label(an, text="X=0 Y=0", bg=PANEL, fg=TEXT,
                               font=("DejaVu Sans", 11, "bold"))
        self.ax_lbl.pack(pady=4)
        for dname, kname in ANALOG:
            b = tk.Button(an, text={"up": "STICK UP", "down": "STICK DOWN",
                                    "left": "STICK LEFT", "right": "STICK RIGHT"}[dname],
                          width=10, bd=1, relief="ridge", bg=PANEL, fg=TEXT,
                          activebackground=GLOW, activeforeground="#1a1200",
                          font=("DejaVu Sans", 9, "bold"))
            b.pack(pady=2)
            b.bind("<ButtonPress-1>", lambda e, dn=dname, k=kname: self._astick(dn, k, True))
            b.bind("<ButtonRelease-1>", lambda e, dn=dname, k=kname: self._astick(dn, k, False))
            b.bind("<Leave>", lambda e, dn=dname, k=kname: self._astick(dn, k, False))

    # ---------- control handlers ----------
    def _down(self, name, kname):
        self.pressed[name] = True
        self.deck.set_button(name, True, inject_kname=kname)
        self.buttons[name].configure(bg=ACCENT, fg="#061a0c")

    def _up(self, name, kname):
        if self.pressed.pop(name, False):
            self.deck.set_button(name, False, inject_kname=kname)
            self.buttons[name].configure(bg=PANEL, fg=TEXT)

    def _astick(self, dname, kname, down):
        self.deck._inject_once(kname, down)
        with self.deck.lock:
            if dname == "up":
                self.deck.ay = -110 if down else 0
            elif dname == "down":
                self.deck.ay = 110 if down else 0
            elif dname == "left":
                self.deck.ax = -110 if down else 0
            elif dname == "right":
                self.deck.ax = 110 if down else 0
            pkt = struct.pack(PKT_FMT, self.deck.mask, self.deck.ax, self.deck.ay,
                              int(time.time() * 1000) & 0xFFFFFFFF)
            try:
                self.deck.udp.sendto(pkt, SERVER_CMD)
            except Exception:
                pass

    # ---------- network ----------
    def _start_network_threads(self):
        def cam_loop():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", CAM_PORT))
            s.settimeout(0.2)
            while True:
                try:
                    data, _ = s.recvfrom(65536)
                except socket.timeout:
                    continue
                if data[:2] != b"\xed\x01" or len(data) < 8:
                    continue
                _, ftype, cam, w, h, size = struct.unpack_from(STREAM_FMT, data[:8])
                payload = data[8:8 + size]
                if ftype == 1:
                    img = payload
                else:
                    try:
                        img = self._rgb565(payload, w, h)
                    except Exception:
                        continue
                self.deck.cam_frames[cam] = (img, w, h)

        def tel_loop():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", TEL_PORT))
            s.settimeout(0.2)
            while True:
                try:
                    data, _ = s.recvfrom(128)
                except socket.timeout:
                    continue
                if len(data) >= struct.calcsize(TELE_FMT):
                    self.deck.telemetry = struct.unpack(TELE_FMT, data[:24])

        threading.Thread(target=cam_loop, daemon=True).start()
        threading.Thread(target=tel_loop, daemon=True).start()

    @staticmethod
    def _rgb565(payload, w, h):
        raw = bytearray(w * h * 3)
        for i in range(w * h):
            p = struct.unpack_from("<H", payload, i * 2)[0]
            raw[i * 3] = ((p >> 11) & 0x1F) << 3
            raw[i * 3 + 1] = ((p >> 5) & 0x3F) << 2
            raw[i * 3 + 2] = (p & 0x1F) << 3
        im = Image.frombytes("RGB", (w, h), bytes(raw))
        buf = __import__("io").BytesIO()
        im.save(buf, format="JPEG", quality=60)
        return buf.getvalue()

    # ---------- UI loop ----------
    def _update_loop(self):
        for cam in (0, 1):
            img = self.deck.cam_frames.get(cam)
            if img:
                raw, w, h = img
                try:
                    im = Image.open(__import__("io").BytesIO(raw))
                    im.thumbnail((460, 300))
                    ph = ImageTk.PhotoImage(im)
                    self.photo[cam] = ph
                    label = getattr(self, f"cam{cam}")
                    label.configure(image=ph)
                except Exception:
                    pass
        t = self.deck.telemetry
        if t:
            btn, ax, ay, mL, mR, _ts = t
            names = [n for n in ("up", "down", "left", "right", "triangle",
                                 "circle", "cross", "square", "L", "R",
                                 "start", "select") if btn & BTN_MASK[n]]
            self.t_btn.configure(text="  buttons: " + (" ".join(names) or "none"))
            self.t_analog.configure(text=f"  analog:  X={ax:4d}  Y={ay:4d}")
            self.mL["value"] = mL + 100
            self.mR["value"] = mR + 100
            self.t_motors.configure(text=f"  L: {mL/100:.0%}   R: {mR/100:.0%}")
        cams = self.deck.cam_frames
        self.t_cams.configure(text=f"  cam frames: 0={1 if cams[0] else 0} 1={1 if cams[1] else 0}")
        self.ax_lbl.configure(text=f"X={self.deck.ax} Y={self.deck.ay}")
        self.after(60, self._update_loop)


def main():
    deck = Deck()
    app = ControlCenter(deck)
    print("[cc] control center up - camera sink 9091, telemetry 9093, mirror 9090")
    app.mainloop()


if __name__ == "__main__":
    main()