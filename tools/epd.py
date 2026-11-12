#!/usr/bin/env python3
"""
epd_ctl 的脚本驱动器。

一次刷固件，之后改测试只改脚本、不用再编译。

用法:
    python tools/epd.py flash                      # 编译 + 烧录 epd_ctl（只需一次）
    python tools/epd.py run scripts/t01_scan.epd   # 跑一个脚本
    python tools/epd.py sh                         # 交互式敲命令

脚本格式（scripts/*.epd）：
    # 注释
    sleep 300                    等待毫秒
    wash 2                       擦屏两次（默认跑脚本前会自动 wash 2）
    其余行原样发给固件

每条命令都会等固件回 "DONE" 再发下一条，保证输出归属不会串。
所有输出同时实时打印并落盘到 logs/run_<时间>.log
"""
import argparse
import datetime as dt
import re
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
SKETCH = "epd_ctl"


def find_port():
    cands = [p for p in list_ports.comports() if p.vid == CH340_VID]
    if not cands:
        return None
    return sorted(cands, key=lambda p: p.device)[0].device


def build_and_flash():
    for d in (ROOT / "build" / SKETCH, ROOT / "build" / f"{SKETCH}_out",
              ROOT / "build" / f"{SKETCH}_default", ROOT / "build" / f"{SKETCH}_default_out"):
        if d.exists():
            import shutil
            shutil.rmtree(d, ignore_errors=True)
    if subprocess.call([sys.executable, str(ROOT / "tools" / "build.py"), SKETCH]) != 0:
        sys.exit("[x] 编译失败")
    out = ROOT / "build" / f"{SKETCH}_default_out"
    bins = list(out.glob("*.bin"))
    if not bins:
        sys.exit("[x] 没找到 bin")
    if subprocess.call([sys.executable, str(ROOT / "tools" / "flash.py"), str(bins[0])]) != 0:
        sys.exit("[x] 烧录失败")


class Board:
    def __init__(self, port, log_path, quiet=False):
        self.port = port
        self.quiet = quiet
        self.log = open(log_path, "w", encoding="utf-8", newline="\n")
        self.ser = None
        self.open()

    def open(self, tries=10):
        for i in range(tries):
            try:
                s = serial.Serial(self.port, 115200, timeout=0.15)
                try:
                    s.setDTR(False)
                    s.setRTS(True)
                    time.sleep(0.12)
                    s.setRTS(False)
                except Exception:
                    pass
                self.ser = s
                return
            except Exception as e:
                if i == 0:
                    print(f"[!] 打开 {self.port} 失败({e})，重试…")
                time.sleep(1.0 + i * 0.4)
        raise SystemExit("[x] 串口打不开，USB 可能掉线了")

    def _emit(self, text):
        if not text:
            return
        self.log.write(text + "\n")
        self.log.flush()
        if not self.quiet:
            print(text)

    def wait_ready(self, timeout=12.0):
        """等固件打印 READY（开串口会复位板子）"""
        t0 = time.time()
        buf = b""
        while time.time() - t0 < timeout:
            try:
                buf += self.ser.read(4096)
            except Exception:
                return False
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self._emit(line)
                if line.strip() == "READY":
                    return True
        return False

    def send(self, cmd, timeout=45.0):
        """发一条命令，读到 DONE 为止；返回这次产生的数据行"""
        self.log.write(f"$ {cmd}\n")
        if not self.quiet:
            print(f"$ {cmd}")
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()

        out = []
        t0 = time.time()
        buf = b""
        while time.time() - t0 < timeout:
            try:
                chunk = self.ser.read(4096)
            except Exception:
                # 掉线：重连并报告
                self._emit("!! 串口断开，尝试重连…")
                time.sleep(2.0)
                self.open(tries=8)
                buf = b""
                t0 = time.time()
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if line.strip() == "DONE":
                    return out
                self._emit(line)
                if line:
                    out.append(line)
        self._emit(f"!! 超时（{timeout:.0f}s 没等到 DONE）：{cmd}")
        return out

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass
        self.log.close()


def parse_script(path):
    steps = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        steps.append(s)
    return steps


def cmd_run(args):
    port = args.port or find_port()
    if not port:
        sys.exit("[x] 找不到 CH340")

    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    name = Path(args.script).stem
    log_path = logs / f"run_{name}_{stamp}.log"

    b = Board(port, log_path, quiet=args.quiet)
    print(f"[+] {port}  ->  {log_path}")
    if not b.wait_ready():
        print("[!] 没等到 READY，照样继续（可能是固件版本不同）")

    b.send("ver", timeout=5)
    if args.wash:
        b.send(f"wash {args.wash}", timeout=60)

    steps = parse_script(args.script)
    t0 = time.time()
    for s in steps:
        if s.startswith("sleep "):
            ms = int(s.split()[1])
            time.sleep(ms / 1000.0)
            continue
        b.send(s, timeout=args.timeout)
    dt_s = time.time() - t0
    b.close()
    print(f"[+] 脚本 {len(steps)} 步完成，用时 {dt_s:.1f}s，日志 {log_path}")


def cmd_sh(args):
    port = args.port or find_port()
    if not port:
        sys.exit("[x] 找不到 CH340")
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    b = Board(port, logs / f"sh_{stamp}.log", quiet=False)
    b.wait_ready()
    print("输入命令，Ctrl-D 退出：")
    try:
        while True:
            try:
                s = input("epd> ").strip()
            except EOFError:
                break
            if not s:
                continue
            b.send(s)
    finally:
        b.close()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("flash", help="编译并烧录 epd_ctl")
    f.set_defaults(func=lambda a: build_and_flash())

    r = sub.add_parser("run", help="跑脚本")
    r.add_argument("script")
    r.add_argument("--port")
    r.add_argument("--wash", type=int, default=2, help="跑前擦屏次数，0=不擦")
    r.add_argument("--timeout", type=float, default=45.0)
    r.add_argument("--quiet", action="store_true")
    r.set_defaults(func=cmd_run)

    s = sub.add_parser("sh", help="交互模式")
    s.add_argument("--port")
    s.set_defaults(func=cmd_sh)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
