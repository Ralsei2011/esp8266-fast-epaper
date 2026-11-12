#!/usr/bin/env python3
"""
自动找 CH340 端口并烧录。

用法:
    python flash.py <bin 路径> [--addr 0x0] [--baud 460800] [--erase]

为什么需要它：这块板子在刷新的瞬间电流拉得比较猛，CH340 容易掉线重枚举，
COM 号会变（COM61 → COM6x）。硬编码端口号每次都要改，不如自动找。

若报「找不到 CH340」，基本就是 USB 掉线了，物理重插即可。
"""
import argparse
import subprocess
import sys

try:
    from serial.tools import list_ports
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

CH340_VID = 0x1A86


def find_ch340():
    candidates = [p for p in list_ports.comports() if p.vid == CH340_VID]
    if not candidates:
        return None, [p for p in list_ports.comports()]
    if len(candidates) > 1:
        print("[!] 检测到多个 CH340，取第一个：", [c.device for c in candidates])
    return candidates[0].device, candidates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("--addr", default="0x0")
    ap.add_argument("--baud", default="460800")
    ap.add_argument("--erase", action="store_true", help="烧录前整片擦除")
    args = ap.parse_args()

    port, all_ports = find_ch340()
    if not port:
        print("[x] 没找到 CH340 设备。")
        if all_ports:
            print("    系统里现有的串口：")
            for p in all_ports:
                print(f"      {p.device}  {p.description}  vid={p.vid}")
        else:
            print("    系统里一个串口都没有 → CH340 已从 USB 掉线。")
            print("    → 请物理重插 USB；仍不行就换一个 USB 口，别走 hub。")
        return 1

    print(f"[+] 使用端口 {port}")
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp8266",
        "--port", port,
        "--baud", args.baud,
        "--before", "default-reset",
        "write-flash",
    ]
    if args.erase:
        cmd += ["--erase-all"]
    cmd += [args.addr, args.bin]

    print("[+] " + " ".join(cmd))
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
