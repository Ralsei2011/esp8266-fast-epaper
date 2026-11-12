#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Bad Apple 墨水屏播放器（PC 端）v2

子命令：
    prep   视频 -> .rlepack
    play   串口推流 + 可选预览窗口

用法:
    python tools/badapple.py prep --fps 30
    python tools/badapple.py play --file media/badapple_30fps.rle --tp 3 --wash 600 --preview
    python tools/badapple.py play --file ... --tp 1 --preview      # 冲 60fps

参数含义：
    --tp     每帧时间片数。1=最快(淡) 3=30fps 6=最黑但只有13fps
    --wash   每多少帧插一次全刷清残影（0=关闭）。全刷约 348ms
"""
import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

CH340_VID = 0x1A86
MAGIC = b"EPDR"
GATES, SOURCES = 296, 128
FBBYTES = GATES * (SOURCES // 8)


# ---------------------------------------------------------------- prep

def cmd_prep(a):
    from mkbadapple import iter_frames, to_bits, rle_pack

    vw = 171
    pad = (GATES - vw) // 2
    out = Path(a.out) if a.out else ROOT / "media" / f"badapple_{int(a.fps)}fps.rle"
    out.parent.mkdir(parents=True, exist_ok=True)

    print(f"预处理 {a.video}  @ {a.fps}fps  {vw}x{SOURCES} 居中于 {GATES}x{SOURCES}")

    frames = []
    t0 = time.time()
    for gray in iter_frames(a.video, a.start, a.dur, a.fps, vw, SOURCES, pad):
        frames.append(rle_pack(to_bits(gray, a.threshold)))
        if len(frames) % 500 == 0:
            print(f"  {len(frames)} 帧 ...", end="\r")

    n = len(frames)
    if n == 0:
        sys.exit("[x] 一帧都没解出来")

    sizes = [len(f) for f in frames]
    avg = sum(sizes) / n
    print(f"  {n} 帧, 平均 {avg:.0f} B/帧, 耗时 {time.time()-t0:.1f}s")

    header_len = 12 + n * 4
    offs, o = [], header_len
    for s in sizes:
        offs.append(o)
        o += s

    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<IHH", n, int(a.fps), 0))
        for off in offs:
            f.write(struct.pack("<I", off))
        for fr in frames:
            f.write(fr)

    print(f"  -> {out}  ({out.stat().st_size/1024/1024:.2f} MB)")
    for b in (921600, 2000000):
        print(f"  单帧传输 @{b} = {avg*8/b*1000:.1f} ms")


# ---------------------------------------------------------------- 预览

def rle_decode(data, fbb=FBBYTES):
    out = bytearray()
    i = 0
    while i < len(data) and len(out) < fbb:
        h = data[i]; i += 1
        cnt = h & 0x7F
        if cnt == 0:
            cnt = 128
        if h & 0x80:
            if i >= len(data):
                break
            v = data[i]; i += 1
            out.extend(bytes([v]) * min(cnt, fbb - len(out)))
        else:
            k = min(cnt, len(data) - i, fbb - len(out))
            out.extend(data[i:i + k])
            i += k
    if len(out) < fbb:
        out.extend(b"\xff" * (fbb - len(out)))
    return bytes(out)


class Preview:
    """本地网页实时预览。这台机器的 Python 没带 tkinter，
    所以用 http.server + multipart 流，浏览器打开 http://127.0.0.1:8777 即可。
    不占串口、不阻塞播放循环。"""

    SCALE = 3
    PORT = 8777

    HTML = """<!doctype html><meta charset="utf-8">
<title>Bad Apple @ EPD</title>
<style>
  body{margin:0;background:#0d0d0f;color:#8ab;font:13px/1.5 Consolas,monospace;
       display:flex;flex-direction:column;align-items:center;justify-content:center;
       min-height:100vh}
  .box{border:1px solid #233;padding:10px;background:#000;
       box-shadow:0 0 40px rgba(80,140,255,.15)}
  img{display:block;image-rendering:pixelated;width:888px;height:384px}
  #s{margin-top:8px;color:#5a7;letter-spacing:.5px}
</style>
<div class="box"><img src="/stream"></div>
<div id="s">connecting…</div>
<script>
setInterval(async()=>{try{const r=await fetch('/status');document.getElementById('s').textContent=await r.text();}catch(e){}},400);
</script>
"""

    def __init__(self, fps):
        import threading
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        from PIL import Image
        import io

        self.Image = Image
        self.io = io
        self.png = b""
        self.status = "starting"
        self.lock = threading.Lock()
        self.running = True
        pv = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path == "/":
                    b = pv.HTML.encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                elif self.path == "/status":
                    b = pv.status.encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain; charset=utf-8")
                    self.send_header("Content-Length", str(len(b)))
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    self.wfile.write(b)
                elif self.path == "/stream":
                    self.send_response(200)
                    self.send_header("Content-Type",
                                     "multipart/x-mixed-replace; boundary=frame")
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    last = None
                    try:
                        while pv.running:
                            with pv.lock:
                                d = pv.png
                            if not d or d is last:
                                time.sleep(0.01)
                                continue
                            last = d
                            self.wfile.write(b"--frame\r\n")
                            self.wfile.write(b"Content-Type: image/png\r\n")
                            self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(d))
                            self.wfile.write(d)
                            self.wfile.write(b"\r\n")
                            time.sleep(0.025)
                    except Exception:
                        pass
                else:
                    self.send_error(404)

        self.srv = ThreadingHTTPServer(("127.0.0.1", self.PORT), H)
        self.srv.daemon_threads = True
        self.th = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.th.start()

        # 编码放到独立线程，否则每帧 3~5ms 的 PNG 编码会把播放帧率拖下来
        import queue
        self.q = queue.Queue(maxsize=1)
        self.worker = threading.Thread(target=self._encode_loop, daemon=True)
        self.worker.start()

    def _encode_loop(self):
        import numpy as np
        while self.running:
            try:
                fb_bytes, text = self.q.get(timeout=0.2)
            except Exception:
                continue
            arr = np.frombuffer(fb_bytes, dtype=np.uint8).reshape(GATES, SOURCES // 8)
            bits = np.unpackbits(arr, axis=1, bitorder="big")   # 1=白
            img = self.Image.fromarray((bits * 255).T.astype(np.uint8), mode="L")
            img = img.resize((GATES * self.SCALE, SOURCES * self.SCALE),
                             self.Image.NEAREST)
            buf = self.io.BytesIO()
            img.save(buf, format="PNG", compress_level=1)
            with self.lock:
                self.png = buf.getvalue()
                self.status = text

    def show(self, fb_bytes, text):
        """只入队，立即返回——绝不阻塞播放循环。"""
        try:
            self.q.put_nowait((fb_bytes, text))
        except Exception:
            pass        # 上一帧还没编完，丢掉这一帧

    def close(self):
        self.running = False
        try:
            self.srv.shutdown()
        except Exception:
            pass


# ---------------------------------------------------------------- play

def find_port():
    from serial.tools import list_ports
    cands = [p for p in list_ports.comports() if p.vid == CH340_VID]
    return sorted(cands, key=lambda p: p.device)[0].device if cands else None


def cmd_play(a):
    import serial

    path = Path(a.file)
    raw = path.read_bytes()
    if raw[:4] != MAGIC:
        sys.exit("[x] 不是 rlepack 文件")
    nframes, fps, _ = struct.unpack("<IHH", raw[4:12])
    offs = [struct.unpack("<I", raw[12 + i * 4:16 + i * 4])[0] for i in range(nframes)]
    print(f"{path.name}: {nframes} 帧, {fps} fps, {len(raw)/1024/1024:.2f} MB")

    i0 = int(a.start * fps) if a.start else 0
    i1 = min(nframes, i0 + int(a.dur * fps)) if a.dur else nframes

    port = a.port or find_port()
    if not port:
        sys.exit("[x] 找不到 CH340 串口")
    print(f"端口 {port} @ {a.baud}, TP={a.tp}, 全刷每 {a.wash} 帧")

    ser = serial.Serial(port, a.baud, timeout=a.timeout)
    ser.reset_input_buffer()

    def wait_ack(tmo, want=b"R"):
        t = time.time()
        while time.time() - t < tmo:
            b = ser.read(1)
            if not b:
                continue
            if b in (b"R", b"W"):
                return True
            if b == b"E":
                return False
            # 其他字符（比如状态行）忽略
        return None

    def probe(timeout=1.5):
        """固件只在 setup 里发一次 'R'，重连串口时不会再发。
        这里用 'Q' 主动问一句，收到任何回复就说明它活着。"""
        ser.reset_input_buffer()
        ser.write(b"\xa5\x5a" + b"Q")
        t = time.time()
        while time.time() - t < timeout:
            b = ser.read(64)
            if b:
                return True
        return False

    print("等固件就绪…", end="", flush=True)
    ok = False
    for _ in range(6):          # 最多等 9 秒（刚上电时要跑 1.65s 的 clearScreen）
        if probe():
            ok = True
            break
    if not ok:
        sys.exit("\n[x] 固件没应答。确认烧的是 epd_play v2，且串口没被别的程序占用")
    print(" 就绪")
    ser.reset_input_buffer()

    # 下发 TP 和全刷间隔
    ser.write(b"\xa5\x5a" + b"T" + bytes([a.tp]))
    wait_ack(3)
    ser.write(b"\xa5\x5a" + b"N" + struct.pack("<H", a.wash))
    wait_ack(3)

    pv = Preview(fps) if a.preview else None
    if pv:
        print(f"预览窗口：http://127.0.0.1:{Preview.PORT}")

    interval = 1.0 / fps
    t_next = time.time()
    t_start = time.time()
    sent = 0
    fails = 0

    try:
        for idx in range(i0, i1):
            o = offs[idx]
            ln = (offs[idx + 1] - o) if idx + 1 < nframes else len(raw) - o
            payload = raw[o:o + ln]

            ser.write(b"\xa5\x5a" + b"F" + struct.pack("<H", ln) + payload)
            r = wait_ack(a.timeout)

            if r is not True:
                fails += 1
                ser.reset_input_buffer()
                if a.strict:
                    print(f"\n[x] 第 {idx} 帧失败，中止")
                    break
                continue

            sent += 1
            if pv:
                el = time.time() - t_start
                cur = sent / max(el, 0.001)
                txt = (f"{sent}/{i1-i0} 帧   {cur:5.1f} fps   "
                       f"目标 {fps:.0f}   {sent/max(el,0.001)/fps*100:3.0f}%   "
                       f"{'全刷' + str(sent // a.wash) if a.wash else '全刷关'}")
                pv.show(rle_decode(payload), txt)

            t_next += interval
            d = t_next - time.time()
            if d > 0:
                time.sleep(d)
            else:
                t_next = time.time()

            if sent % 60 == 0 or idx == i1 - 1:
                el = time.time() - t_start
                print(f"  {sent}/{i1-i0} 帧  {sent/el:.1f} fps  "
                      f"{sent/el/fps*100:.0f}%  失败{fails}", flush=True)

    except KeyboardInterrupt:
        print("\n[中断]")
    except Exception as e:
        print(f"\n[!] 串口异常：{type(e).__name__}: {e}")

    el = time.time() - t_start
    print(f"\n完成：{sent} 帧 / {el:.1f}s = {sent/max(el,0.001):.1f} fps（目标 {fps}）")

    if pv and a.hold:
        print(f"预览保持：http://127.0.0.1:{Preview.PORT}   （Ctrl+C 退出）")
        try:
            while True:
                time.sleep(0.2)
        except KeyboardInterrupt:
            pass

    if pv:
        pv.close()
    ser.close()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("prep")
    p.add_argument("--video", default=str(ROOT / "media" / "badapple.f30033.mp4"))
    p.add_argument("--start", type=float, default=0)
    p.add_argument("--dur", type=float, default=0)
    p.add_argument("--fps", type=float, default=30)
    p.add_argument("--threshold", type=int, default=128)
    p.add_argument("--out", default="")
    p.set_defaults(func=cmd_prep)

    q = sub.add_parser("play")
    q.add_argument("--file", required=True)
    q.add_argument("--port", default="")
    q.add_argument("--baud", type=int, default=2000000)
    q.add_argument("--tp", type=int, default=3)
    q.add_argument("--wash", type=int, default=600)
    q.add_argument("--start", type=float, default=0)
    q.add_argument("--dur", type=float, default=0)
    q.add_argument("--timeout", type=float, default=2.0)
    q.add_argument("--preview", action="store_true")
    q.add_argument("--hold", action="store_true")
    q.add_argument("--strict", action="store_true")
    q.set_defaults(func=cmd_play)

    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
