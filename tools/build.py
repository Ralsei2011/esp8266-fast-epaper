#!/usr/bin/env python3
"""
一键编译固件（arduino-cli 封装）。

用法:
    python tools/build.py --list                    # 列出所有可编译的 sketch
    python tools/build.py epd_ctl
    python tools/build.py epd_play --flash
    python tools/build.py epd_wave_abc --clean      # 强制全量重编
    python tools/build.py epd_play --fs 16M14M      # 换过更大 flash 的板子

为什么需要它：
  1. 自动在 firmware/ 和 research/3color/ 下定位 sketch，不用记路径；
  2. arduino-cli 的增量缓存有时候不拾取源码改动（改了源码却跑旧固件），
     --clean 会先删掉 build 目录强制全量重编 —— 这个坑踩过不止一次；
  3. 编译产物路径固定为 build/<sketch>_default_out/，方便别的脚本引用。

产物：build/<sketch>_default_out/<sketch>.ino.bin
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARDUINO_CLI = ROOT / "tools" / "arduino-cli.exe"
CONFIG = ROOT / "tools" / "cli-config.yaml"

# flash 容量与文件系统划分。默认按最常见的 4MB NodeMCU 板。
# eesz 的完整取值见 `arduino-cli board details -b esp8266:esp8266:generic`：
#   4M2M   = 4MB flash + 2MB FS + OTA 约 1MB（默认）
#   4M3M   = 4MB flash + 3MB FS，OTA 只剩 512KB
#   16M14M = 16MB flash + 14MB FS，OTA 约 1MB
# 换过 flash 芯片的板子必须改这一项，否则固件头里写的容量和实际芯片对不上。
DEFAULT_FS = "4M2M"


SKETCH_ROOTS = (ROOT, ROOT / "firmware", ROOT / "research" / "3color")


def find_sketch(name: str) -> Path:
    """支持 `epd_ctl`、`firmware/epd_ctl`、`research/3color/epd_demo` 等写法。"""
    for base in SKETCH_ROOTS:
        cand = base / name
        if cand.is_dir() and list(cand.glob("*.ino")):
            return cand
    sys.exit(f"[x] 找不到 sketch：{name}（试过 firmware/ 和 research/3color/）")


def list_sketches() -> list:
    out = []
    for base in SKETCH_ROOTS[1:]:
        if base.is_dir():
            for d in sorted(base.iterdir()):
                if d.is_dir() and list(d.glob("*.ino")):
                    out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sketch", nargs="?", help="sketch 名字，例如 epd_ctl / epd_wave_abc")
    ap.add_argument("--list", action="store_true", help="列出全部可编译的 sketch")
    ap.add_argument("--flash", action="store_true", help="编译完直接烧录")
    ap.add_argument("--clean", action="store_true", help="先删 build 目录，强制全量重编")
    ap.add_argument("--baud", default="460800", help="烧录波特率")
    ap.add_argument("--fs", default=DEFAULT_FS,
                    help=f"flash 容量/文件系统划分，默认 {DEFAULT_FS}；换过 flash 芯片的板子要改")
    args = ap.parse_args()

    fqbn = f"esp8266:esp8266:generic:eesz={args.fs}"

    if args.list:
        for d in list_sketches():
            print(d.relative_to(ROOT).as_posix())
        return

    if not args.sketch:
        ap.error("需要 sketch 名字，或用 --list 查看全部")

    sketch_dir = find_sketch(args.sketch)
    name = sketch_dir.name
    build_path = ROOT / "build" / f"{name}_default"
    out_path = ROOT / "build" / f"{name}_default_out"

    # 换了 --fs 就必须全量重编：同一个 build 目录里缓存的 .o 是按旧容量编的，
    # 直接复用会让编译器在链接阶段崩掉（Windows 上是 0xc0000005 访问违例，
    # 看起来像代码错误，其实是缓存脏了）。用标记文件自动判断。
    marker = build_path / ".eesz"
    stale = marker.is_file() and marker.read_text().strip() != args.fs
    if stale:
        print(f"[i] flash 配置从 {marker.read_text().strip()} 变成 {args.fs}，清缓存全量重编")

    if args.clean or stale:
        for d in (build_path, out_path):
            shutil.rmtree(d, ignore_errors=True)
        print("[i] 已清空 build 缓存")

    cmd = [
        str(ARDUINO_CLI), "compile",
        "--config-file", str(CONFIG),
        "--fqbn", fqbn,
        "--build-path", str(build_path),
        "--output-dir", str(out_path),
        "-j", "0",
        str(sketch_dir),
    ]
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.exit("[x] 编译失败")

    build_path.mkdir(parents=True, exist_ok=True)
    marker.write_text(args.fs)

    bins = list(out_path.glob("*.bin"))
    if not bins:
        sys.exit("[x] 编译成功但没找到 bin")

    print(f"[+] 产物 {bins[0]}")
    if args.flash:
        rc = subprocess.call([sys.executable, str(ROOT / "tools" / "flash.py"),
                              str(bins[0]), "--baud", args.baud])
        if rc != 0:
            sys.exit("[x] 烧录失败")


if __name__ == "__main__":
    main()
