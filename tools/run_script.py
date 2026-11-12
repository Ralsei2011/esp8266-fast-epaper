#!/usr/bin/env python3
"""
把一份脚本推给 epd_vm 并执行，实时把每步耗时记下来。

用法：
    python tools/run_script.py scripts/win_matrix.epd w8
    python tools/run_script.py scripts/win_matrix.epd w8 --panel 5

流程：PUT <name> <len> + 正文 -> RUN <name> -> 抓 TRIG/PRINT 输出。
脚本里用 TRIG 0xF7 这种数字写法，不能写 f7（参数走 hexOrDec）。
"""
import argparse
import pathlib
import re
import serial
import serial.tools.list_ports as lp
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def find_port():
    for p in lp.comports():
        if "CH340" in (p.description or "").upper():
            return p.device
    ports = [p.device for p in lp.comports()]
    if ports:
        return ports[0]
    sys.exit("[x] 找不到串口")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("name", help="脚本名（<=40 字节）")
    ap.add_argument("--port", default="")
    ap.add_argument("--panel", default="?", help="屏号，写进结果文件")
    args = ap.parse_args()

    body = pathlib.Path(args.file).read_bytes()
    if len(body) > 4000:
        sys.exit(f"[x] {len(body)} 字节，超过 4000 上限")
    if len(args.name) > 40:
        sys.exit("[x] 脚本名太长")

    port = args.port or find_port()
    print(f"[+] {port}  {args.file}  {len(body)} bytes  ->  {args.name}")

    s = serial.Serial(port, 2000000, timeout=0.5)
    s.reset_input_buffer()
    time.sleep(1.0)
    s.write(b"\n")
    s.flush()
    time.sleep(0.3)

    def send(text, wait_for, timeout):
        s.write((text + "\n").encode())
        s.flush()
        t0 = time.time()
        buf = b""
        while time.time() - t0 < timeout:
            d = s.read(300)
            if d:
                buf += d
                if wait_for in buf:
                    break
        return buf.decode(errors="replace")

    # 上传
    r = send(f"PUT {args.name} {len(body)}", b"READY", 5)
    if "READY" not in r:
        sys.exit(f"[x] 固件没准备接收：{r!r}")
    s.write(body)
    s.flush()
    time.sleep(0.6)
    r = send("", b"OK PUT", 5)
    print("    PUT ->", r.strip().splitlines()[-1] if r.strip() else "(?)")

    # 执行
    print("[i] RUN，全程约 2 分钟...")
    t0 = time.time()
    buf = b""
    s.write((f"RUN {args.name}\n").encode())
    s.flush()
    while time.time() - t0 < 240:
        d = s.read(400)
        if d:
            buf += d
            txt = buf.decode(errors="replace")
            if "OK RUN" in txt or "ERR" in txt and "RUN" in txt:
                break
    s.close()

    out = buf.decode(errors="replace")
    lines = [x for x in out.splitlines() if x.strip() and "expActivate" not in x]
    for l in lines:
        print("   ", l)

    trigs = re.findall(r"TRIG 0x([0-9A-Fa-f]{2}) (\d+) ms", out)
    if trigs:
        path = ROOT / "results" / f"vm_matrix_panel{args.panel}.csv"
        path.parent.mkdir(exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write("step,mode,ms\n")
            for i, (m, ms) in enumerate(trigs, 1):
                f.write(f"{i},{m},{ms}\n")
        print(f"[+] 记录 {len(trigs)} 次刷新 -> {path}")
        print("    " + "  ".join(f"{ms}ms" for _, ms in trigs))


if __name__ == "__main__":
    main()
