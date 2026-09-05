#!/usr/bin/env python3
"""
Teaching Deck Interactive Lab
=============================
A web dashboard that lets YOU drive the whole system from your browser:

  [browser: virtual PSP deck]
        |  POST /cmd  (button mask + analog)
        v
  lab.py  ---UDP#9090-->  server.py (test program: reads + relays)
                                    | forward
                                    v
                          robot_agent.py (simulated robot)
                             | camera JPEG UDP -> lab.py :9091 (MJPEG served to browser)
                             | telemetry  UDP -> lab.py :9093 (motors/state)

Use it:  python3 test/lab.py          # then open http://127.0.0.1:8080
(see test/run_lab.sh to start server.py + robot_agent.py too)
"""

import argparse
import io
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BUTTON_MASK = {
    "select": 0x1, "start": 0x8, "up": 0x10, "right": 0x20,
    "down": 0x40, "left": 0x80, "L": 0x100, "R": 0x200,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
}

STREAM_MAGIC = b"\xED\x01"
FRAME_JPEG, FRAME_RGB565 = 1, 2

TELEMETRY_FMT = "<IiiiiI"           # buttons, ax, ay, motorL, motorR, ts
BUTTON_PKT_FMT = "<IiiI"            # PSP deck -> server: same as the real deck

DECK_ADDR = ("127.0.0.1", 9090)     # server.py UDP command port


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.telemetry = None        # last robot telemetry packet (tuple)
        self.motors = (0.0, 0.0)
        self.cam_frames = {0: None, 1: None}   # cam -> (jpeg bytes, gen)
        self.cam_meta = {}
        self.lock_ = self.lock

    def set_frame(self, cam, jpeg):
        with self.lock:
            self.cam_frames[cam] = jpeg
            self.cam_meta[cam] = self.cam_meta.get(cam, 0) + 1

    def set_telemetry(self, t):
        with self.lock:
            self.telemetry = t
            if t:
                self.motors = (t[3] / 100.0, t[4] / 100.0)  # percent

    def snapshot(self):
        with self.lock:
            t = self.telemetry
            return {
                "connected": bool(t),
                "buttons": t[0] if t else 0,
                "analog": [t[1], t[2]] if t else [0, 0],
                "motor_percent": list(self.motors),
                "cam_frames_received": dict(self.cam_meta),
            }


def rgb565_to_jpeg(payload, w, h):
    from PIL import Image
    raw = Image.new("RGB", (w, h))
    pix = raw.load()
    for y in range(h):
        for x in range(w):
            p = struct.unpack_from("<H", payload, (y * w + x) * 2)[0]
            r = (p >> 11) & 0x1F
            g = (p >> 5) & 0x3F
            b = p & 0x1F
            pix[x, y] = (r << 3, g << 2, b << 3)
    buf = io.BytesIO()
    raw.save(buf, format="JPEG", quality=60)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description="Interactive teaching-deck lab")
    ap.add_argument("--http", type=int, default=8080)
    ap.add_argument("--video-port", type=int, default=9091,
                    help="robot camera UDP sink (used by robot_agent --sink-port)")
    ap.add_argument("--telemetry-port", type=int, default=9093,
                    help="robot telemetry UDP (used by robot_agent --telemetry-to)")
    ap.add_argument("--cmd-port", type=int, default=9090,
                    help="server.py command reader port we inject deck packets into")
    args = ap.parse_args()

    global DECK_ADDR
    DECK_ADDR = ("127.0.0.1", args.cmd_port)
    state = State()

    cmd_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # ---- camera sink thread: robot -> lab -> MJPEG served in HTTP
    def video_loop():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", args.video_port))
        s.settimeout(0.2)
        while True:
            try:
                data, _ = s.recvfrom(65536)
            except socket.timeout:
                continue
            if data[:2] != STREAM_MAGIC or len(data) < 8:
                continue
            _, ftype, cam, w, h, size = struct.unpack_from("<2sBBBBH", data[:8])
            payload = data[8:8 + size]
            jpeg = payload if ftype == FRAME_JPEG else None
            if jpeg is None:
                try:
                    jpeg = rgb565_to_jpeg(payload, w, h)
                except Exception:
                    continue
            if jpeg:
                state.set_frame(cam, jpeg)

    # ---- telemetry sink thread: robot -> lab
    def telemetry_loop():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", args.telemetry_port))
        s.settimeout(0.2)
        while True:
            try:
                data, _ = s.recvfrom(128)
            except socket.timeout:
                continue
            if len(data) >= struct.calcsize(TELEMETRY_FMT):
                state.set_telemetry(struct.unpack(TELEMETRY_FMT, data[:24]))

    threading.Thread(target=video_loop, daemon=True).start()
    threading.Thread(target=telemetry_loop, daemon=True).start()

    # ===================== HTTP =====================
    class LabHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                body = PAGE.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif path == "/state":
                st = state.snapshot()
                st["deck_addr"] = list(DECK_ADDR)
                self._json(st)
            elif path.startswith("/mjpeg/"):
                cam = int(path.split("/")[-1])
                self._mjpeg_stream(cam)
            elif path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
            else:
                self.send_response(404)
                self.end_headers()

        def do_POST(self):
            if self.path.split("?")[0] != "/cmd":
                self.send_response(404)
                self.end_headers()
                return
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except Exception:
                body = {}
            mask = int(body.get("b", 0))
            x = max(-127, min(127, int(body.get("x", 0))))
            y = max(-127, min(127, int(body.get("y", 0))))
            ts = int(time.time() * 1000) & 0xFFFFFFFF
            cmd_sock.sendto(struct.pack(BUTTON_PKT_FMT, mask, x, y, ts), DECK_ADDR)
            self._json({"ok": True, "sent": [mask, x, y]})

        def _mjpeg_stream(self, cam):
            self.send_response(200)
            self.send_header("Content-Type",
                             'multipart/x-mixed-replace; boundary=teachdeck')
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            last = -1
            try:
                while True:
                    with state.lock:
                        jpeg = state.cam_frames.get(cam)
                    if jpeg is not None:
                        # newest frame; gen is the object identity when repacked
                        gen = id(jpeg)
                        if gen != last:
                            last = gen
                            self.wfile.write(
                                b"--teachdeck\r\nContent-Type: image/jpeg\r\n"
                                b"Content-Length: " + str(len(jpeg)).encode() +
                                b"\r\n\r\n" + jpeg + b"\r\n")
                            self.wfile.flush()
                    time.sleep(0.05)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET_body(self, *a):
            pass  # placeholder never used

    srv = ThreadingHTTPServer(("0.0.0.0", args.http), LabHandler)
    print(f"[LAB] interactive lab   ->  http://127.0.0.1:{args.http}")
    print(f"[LAB] camera sink       ->  UDP :{args.video_port}  (robot --sink-port {args.video_port})")
    print(f"[LAB] telemetry sink    ->  UDP :{args.telemetry_port}  (robot --telemetry-to 127.0.0.1:{args.telemetry_port})")
    print(f"[LAB] deck packets      ->  UDP :{args.cmd_port}  (server.py --port {args.cmd_port})")
    print("[LAB] open the URL and click the PSP buttons / drag the analog stick.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[LAB] bye")


PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<title>Teaching Deck - Interactive Lab</title><style>
:root{--bg:#0b0d12;--fg:#cfe3cf;--acc:#3ddc55;--dim:#5c6b5c;--panel:#151a22}
*{box-sizing:border-box;font-family:ui-monospace,Menlo,Consolas,monospace}
body{margin:0;background:var(--bg);color:var(--fg);padding:18px;display:flex;gap:18px;flex-wrap:wrap}
h1{font-size:15px;margin:0 0 12px;color:var(--acc);letter-spacing:2px;text-transform:uppercase}
.panel{background:var(--panel);border:1px solid #222a33;border-radius:10px;padding:14px;min-width:260px}
.feed{background:#000;border-radius:6px;overflow:hidden;position:relative}
.feed img{display:block;width:320px;height:240px;image-rendering:pixelated;background:#000}
.feed .tag{position:absolute;top:6px;left:6px;background:#000a;color:var(--acc);font-size:11px;padding:2px 6px;border-radius:4px}
.tele{font-size:13px;line-height:1.7}
.tele b{color:var(--acc)}
#deck{position:relative;width:340px;height:330px;background:#0e1524;border-radius:14px;border:1px solid #1d2a3f}
.btn{position:absolute;width:30px;height:30px;border-radius:50%;background:#1c2633;border:1px solid #3a4857;display:flex;align-items:center;justify-content:center;font-size:12px;color:#9fb2c7;cursor:pointer;user-select:none;font-weight:bold}
.btn:active,.btn.down{background:var(--acc);color:#062;border-color:var(--acc)}
#dpad{position:absolute;left:30px;top:90px;width:100px;height:100px}
.dpad{position:absolute;width:32px;height:32px;background:#1c2633;border:1px solid #2a3850;display:flex;align-items:center;justify-content:center;color:#9fb2c7;font-size:9px;cursor:pointer;font-weight:bold}
.dpad.down{background:var(--acc);color:#062}
#tube{display:none}
#stick{position:absolute;left:115px;top:250px;cursor:grab;user-select:none;touch-action:none}
#statebox{position:absolute;left:225px;top:10px;font-size:11px;color:var(--dim);line-height:1.5}
#right{position:absolute;left:286px;top:30px}
.db{position:absolute;width:34px;height:34px;border-radius:6px;background:#1c2633;border:1px solid #2a3850;color:#9fb2c7;display:flex;align-items:center;justify-content:center;font-size:10px;cursor:pointer;font-weight:bold}
.db.down{background:#e94f4f;color:#000}
#log{font-size:11px;color:var(--dim);white-space:pre-wrap;height:90px;overflow-y:auto;margin-top:8px}
</style></head><body>
<div class="panel">
  <h1>&#9679; Teaching Deck &mdash; Interactive Lab</h1>
  <div class="feed"><img id="cam0" src="/mjpeg/0"><span class="tag">robot cam 0</span></div>
  <div class="feed" style="margin-top:10px"><img id="cam1" src="/mjpeg/1"><span class="tag">robot cam 1</span></div>
</div>
<div class="panel" style="width:360px">
  <h1>&#9632; Virtual PSP deck</h1>
  <div id="deck">
    <div style="position:absolute;left:26px;top:14px;font-size:10px;color:var(--dim)">L</div>
    <div style="position:absolute;right:26px;top:14px;font-size:10px;color:var(--dim)">R</div>
    <div id="dpad">
      <div class="dpad" id="up" style="left:34px;top:0">UP</div>
      <div class="dpad" id="down" style="left:34px;top:68px">DN</div>
      <div class="dpad" id="left" style="left:0;top:34px">LT</div>
      <div class="dpad" id="right" style="left:68px;top:34px">RT</div>
    </div>
    <div class="btn" id="triangle" style="left:234px;top:72px">&#9650;</div>
    <div class="btn" id="circle" style="left:282px;top:96px">&#9675;</div>
    <div class="btn" id="cross" style="left:210px;top:96px">&#10005;</div>
    <div class="btn" id="square" style="left:234px;top:120px">&#9633;</div>
    <div id="statebox">
      analog<br><span id="ax">0</span>,<span id="ay">0</span><br>
      buttons <span id="hex">0x0</span>
    </div>
    <canvas id="stick" width="110" height="110"></canvas>
    <div class="db" id="select" style="left:100px;top:286px">SELECT</div>
    <div class="db" id="start" style="left:200px;top:286px">START</div>
  </div>
  <div id="log"></div>
</div>
<div class="panel tele">
  <h1>&#9878; Robot telemetry</h1>
  connected <b id="t_conn">NO</b><br>
  cmd rx <b id="t_btn">0x0 none</b><br>
  analog <b id="t_ax">0</b> , <b id="t_ay">0</b><br>
  motor <b id="t_l">0</b> , <b id="t_r">0</b>  (% power)<br>
  deck link <b id="t_addr">-</b><br>
  cams rx <b id="t_cams">-</b>
</div>
<script>
const M={select:1,start:8,up:16,right:32,down:64,left:128,L:256,R:512,triangle:4096,circle:8192,cross:16384,square:32768};
let mask=0, x=0, y=0, lastSent='';
const names=['SELECT','START','UP','RIGHT','DOWN','LEFT','L','R','TRIANGLE','CIRCLE','CROSS','SQUARE'];
function btnName(m){return names.filter((n,i)=>m&[1,8,16,32,64,128,256,512,4096,8192,16384,32768][i]).join('+')||'none';}
function setBtn(id,pressed){
  const el=document.getElementById(id); if(!el)return;
  el.classList.toggle('down',pressed);
  mask = pressed ? (mask | M[id]) : (mask & ~M[id]);
  document.getElementById('hex').textContent='0x'+mask.toString(16);
}
document.querySelectorAll('.btn,.dpad,.db').forEach(el=>{
  const id=el.id; if(!M[id])return;
  const on=e=>{e.preventDefault();setBtn(id,true);};
  const off=e=>{e.preventDefault();setBtn(id,false);};
  el.addEventListener('pointerdown',on);
  el.addEventListener('pointerup',off);
  el.addEventListener('pointerleave',off);
  el.addEventListener('pointercancel',off);
});
// analog stick
const c=document.getElementById('stick'), ctx=c.getContext('2d');
let r=44, dx=0, dy=0;
function draw(){
  ctx.clearRect(0,0,c.width,c.height);
  ctx.beginPath();ctx.arc(c.width/2,c.height/2,r,0,7);ctx.strokeStyle='#1d2a3f';ctx.stroke();
  ctx.beginPath();ctx.arc(c.width/2+dx,c.height/2+dy,14,0,7);ctx.fillStyle='#3ddc55';ctx.fill();
  x=Math.max(-127,Math.min(127,Math.round(dx/r*127)));
  y=Math.max(-127,Math.min(127,Math.round(dy/r*127)));
  document.getElementById('ax').textContent=x;document.getElementById('ay').textContent=y;
}
c.addEventListener('pointerdown',e=>c.setPointerCapture(e.pointerId));
c.addEventListener('pointermove',e=>{
  const b=c.getBoundingClientRect(),cx=c.width/2,cy=c.height/2;
  let px=(e.clientX-b.left)-cx, py=(e.clientY-b.top)-cy;
  const d=Math.hypot(px,py); if(d>r){px=px/d*r;py=py/d*r;}
  dx=px;dy=py;draw();
});
c.addEventListener('pointerup',()=>{dx=0;dy=0;draw();});
draw();
// cadence: push the deck state into the real pipeline at ~10 Hz
setInterval(()=>{
  const s=mask+','+x+','+y;
  if(s!==lastSent){
    lastSent=s;
    fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
                  body:JSON.stringify({b:mask,x,y})});
    const log=document.getElementById('log');
    log.textContent=(new Date().toISOString().slice(11,19))+' 0x'+mask.toString(16)
       +' analog '+x+','+y+'\n'+log.textContent.slice(0,600);
  }
},120);
// telemetry poll
setInterval(async()=>{
  try{
    const st=await (await fetch('/state')).json();
    document.getElementById('t_conn').textContent=st.connected?'YES':'NO';
    document.getElementById('t_btn').textContent='0x'+(st.buttons||0).toString(16)+' '+btnName(st.buttons||0);
    const a=st.analog||[0,0];
    document.getElementById('t_ax').textContent=a[0];
    document.getElementById('t_ay').textContent=a[1];
    if(st.motor_percent){document.getElementById('t_l').textContent=st.motor_percent[0].toFixed(1);
      document.getElementById('t_r').textContent=st.motor_percent[1].toFixed(1);}
    document.getElementById('t_addr').textContent=(st.deck_addr||[]).join(':');
    document.getElementById('t_cams').textContent=JSON.stringify(st.cam_frames_received||{});
  }catch(e){}
},400);
</script></body></html>"""

if __name__ == "__main__":
    main()