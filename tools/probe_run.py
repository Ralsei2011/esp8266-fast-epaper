#!/usr/bin/env python3
"""
烧录 + 全程抓串口日志（一次跑完，不丢一行）。

用法:
    python tools/probe_run.py epd_a01_probe --secs 120
    python tools/probe_run.py epd_a01_probe --secs 120 --no-build

为什么单独写它：
  1. 这块板子的 CH340 在刷新瞬间容易掉线重枚举，串口要重试打开；
  2. 探测固件会把 31 档的结果一口气打出来，人工看终端会漏行，
     所以这里一边实时打印一边落盘到 logs/probe_<时间>.log。
"""
import argparse
import datetime as dt
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

ROOT = Path(__file__).resolve().parent.parent
CH340_VID = 0x1A86


def find_port():
    cands = [p for p in list_ports.comports() if p.vid == CH340_VID]
    if not cands:
        return None
    return sorted(cands, key=lambda p: p.device)[0].device


def build(sketch):
    for d in (ROOT / "build" / sketch, ROOT / "build" / f"{sketch}_out",
              ROOT / "build" / f"{sketch}_default", ROOT / "build" / f"{sketch}_default_out"):
        subprocess.call(["rm", "-rf", str(d)], shell=False)
    rc = subprocess.call([sys.executable, str(ROOT / "tools" / "build.py"), sketch])
    if rc != 0:
        sys.exit("[x] 编译失败")


def flash(sketch):
    out = ROOT / "build" / f"{sketch}_default_out"
    bins = list(out.glob("*.bin"))
    if not bins:
        sys.exit("[x] 没找到 bin")
    rc = subprocess.call([sys.executable, str(ROOT / "tools" / "flash.py"), str(bins[0])])
    if rc != 0:
        sys.exit("[x] 烧录失败")


def open_port(port, tries=12):
    for i in range(tries):
        try:
            s = serial.Serial(port, 115200, timeout=0.2)
            return s
        except Exception as e:
            print(f"[!] 打开 {port} 失败({e})，{1.0+i*0.5:.1f}s 后重试 {i+1}/{tries}")
            time.sleep(1.0 + i * 0.5)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sketch")
    ap.add_argument("--secs", type=float, default=120.0)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-flash", action="store_true")
    ap.add_argument("--port")
    ap.add_argument("--stop-marker", default="=== PROBE END ===")
    args = ap.parse_args()

    if not args.no_build:
        print("[i] 编译…")
        build(args.sketch)
    if not args.no_flash:
        print("[i] 烧录…")
        flash(args.sketch)
        time.sleep(1.5)

    port = args.port or find_port()
    if not port:
        sys.exit("[x] 找不到 CH340，USB 可能掉线了，物理重插一下")

    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = logs / f"probe_{stamp}.log"

    ser = open_port(port)
    if not ser:
        sys.exit("[x] 串口打不开")

    print(f"[+] {port}  {log_path}\n" + "-" * 60)

    # 复位进正常启动（DTR/RTS 组合，NodeMCU 那种自动复位电路）
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        time.sleep(0.05)
    except Exception:
        pass
    ser.reset_input_buffer()

    t0 = time.time()
    seen_end = False
    end_at = None
    nlines = 0
    buf = b""

    with open(log_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# probe log  port={port}  sketch={args.sketch}  {stamp}\n")
        while True:
            now = time.time()
            if now - t0 > args.secs:
                break
            try:
                chunk = ser.read(4096)
            except Exception as e:
                print(f"\n[!] 读串口异常({e})，重连…")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2.0)
                ser = open_port(port, tries=6)
                if not ser:
                    print("[x] 重连失败，停止")
                    break
                continue

            if not chunk:
                if seen_end and end_at and now - end_at > 3.0:
                    break
                continue

            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                print(text)
                f.write(text + "\n")
                f.flush()
                nlines += 1
                if args.stop_marker and args.stop_marker in text:
                    seen_end = True
                    end_at = time.time()

    try:
        ser.close()
    except Exception:
        pass
    print("-" * 60)
    print(f"[+] 共 {nlines} 行，日志：{log_path}")


if __name__ == "__main__":
    main()
