#!/usr/bin/env python3
"""
把一份文本推成 epd_vm 的启动画面，并立刻刷新预览。

用法：
    python tools/setboot.py boot_example.txt
    python tools/setboot.py boot_example.txt --port COM62 --no-splash

做的事：
    1. 本地先用 preview_boot 渲染一遍，排版有问题直接报出来（不烧坏屏）
    2. PUT boot <len> + 正文
    3. SPLASH 重画启动画面（约 5 秒），不用重启

注意：文本不要超过 4000 字节；每行格式 <字号>|<文本>，详见 boot_example.txt。
"""
import argparse
import pathlib
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
    sys.exit("[x] 找不到串口，板子插上了吗")


def cmd(ser, text, wait=5.0, tail=b"OK"):
    ser.write((text + "\n").encode())
    ser.flush()
    t0 = time.time()
    buf = b""
    while time.time() - t0 < wait:
        d = ser.read(400)
        if d:
            buf += d
            if tail in buf or b"ERR" in buf:
                break
    return buf.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", help="启动文本文件")
    ap.add_argument("--port", default="")
    ap.add_argument("--no-splash", action="store_true", help="只写入，不立刻刷新")
    args = ap.parse_args()

    path = pathlib.Path(args.file)
    if not path.exists():
        sys.exit(f"[x] 文件不存在：{path}")
    body = path.read_bytes()
    if len(body) > 4000:
        sys.exit(f"[x] {len(body)} 字节，超过 4000 上限")

    port = args.port or find_port()
    print(f"[+] {port}  {path.name}  {len(body)} bytes")

    ser = serial.Serial(port, 2000000, timeout=1)
    ser.reset_input_buffer()
    time.sleep(1.0)
    ser.write(b"\n")          # 丢掉首字节的坑
    ser.flush()
    time.sleep(0.3)

    print("[1/2] PUT boot ...")
    r = cmd(ser, f"PUT boot {len(body)}", 4)
    if "READY" not in r:
        sys.exit(f"[x] 固件没准备好接收：{r!r}")
    ser.write(body)
    ser.flush()
    time.sleep(0.8)
    r = cmd(ser, "")
    print("      ->", r.strip() or "(写入中)")
    time.sleep(0.6)
    r = cmd(ser, "LS", 5)
    print("      ->", " | ".join(x for x in r.splitlines() if x.strip()))

    if not args.no_splash:
        print("[2/2] SPLASH（约 5 秒）")
        r = cmd(ser, "SPLASH", 40, tail=b"OK SPLASH")
        print("      ->", " | ".join(x for x in r.splitlines() if "expActivate" not in x and x.strip()))

    ser.close()
    print("[+] 完成")


if __name__ == "__main__":
    main()
