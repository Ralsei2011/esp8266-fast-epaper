#!/usr/bin/env python3
"""
把 driver/exp_api.* 注入到 GxEPD2 库，让固件能直接操作 IL3820 的低层寄存器。

为什么需要它：
    GxEPD2 的公开 API 每一步都带 _Init_Part / _Init_Full 副作用，会重新下发 LUT、
    温度、扫描时序，把我们要保持的状态全盖掉。想「只改一个寄存器然后单独触发一次
    扫描」，必须直接写命令。exp_api 就是这个后门。

集成方式（README 里手写的那两步，这里自动化）：
    1. 复制 driver/exp_api.h、driver/exp_api.cpp 到 <GxEPD2>/src/epd/
    2. GxEPD2_290.h 里加两处：#include "exp_api.h" + class 内 EPD_EXP_DECLS

用法:
    python tools/patch_gxepd2.py --lib libs/GxEPD2
    python tools/patch_gxepd2.py --lib libs/GxEPD2 --check   # 只检查不修改

幂等：重复执行不会重复插入；只加两行，不删改库的任何原有内容。
"""
import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "driver"
MARK_INCLUDE = '#include "exp_api.h"'
MARK_DECLS = "EPD_EXP_DECLS"


def patch_header(path: Path) -> str:
    """在 GxEPD2_290.h 里插两处。返回动作描述。"""
    text = path.read_text(encoding="utf-8")

    if MARK_INCLUDE in text and MARK_DECLS in text:
        return "已注入过，跳过"

    lines = text.splitlines(keepends=True)
    out = []
    did_inc = False
    did_decl = False
    in_class = False

    for line in lines:
        # 1) class 声明之前插 include
        if (not did_inc) and line.lstrip().startswith("class GxEPD2_290 "):
            out.append(MARK_INCLUDE + "  // patched: 低层寄存器接口\n")
            did_inc = True
            in_class = True
            out.append(line)
            continue
        # 2) class 内第一个 public: 之后插宏
        if in_class and (not did_decl) and line.strip() == "public:":
            out.append(line)
            out.append("    " + MARK_DECLS + "  // patched\n")
            did_decl = True
            continue
        out.append(line)

    if not did_inc or not did_decl:
        raise SystemExit(f"[x] 在 {path} 里没找到插入点，GxEPD2 版本可能不对")

    path.write_text("".join(out), encoding="utf-8")
    return "已插入 include + EPD_EXP_DECLS"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True, help="GxEPD2 库根目录，例如 libs/GxEPD2")
    ap.add_argument("--check", action="store_true", help="只检查，不写文件")
    args = ap.parse_args()

    lib = Path(args.lib).expanduser().resolve()
    target_dir = lib / "src" / "epd"
    header = target_dir / "GxEPD2_290.h"
    if not header.is_file():
        sys.exit(f"[x] 找不到 {header}，--lib 指向的是 GxEPD2 根目录吗？")

    text = header.read_text(encoding="utf-8")
    if MARK_INCLUDE in text and MARK_DECLS in text:
        print(f"[=] {lib} 已经注入过")
        return

    if args.check:
        print(f"[!] {lib} 尚未注入")
        sys.exit(1)

    for name in ("exp_api.h", "exp_api.cpp"):
        src = DRIVER / name
        if not src.is_file():
            sys.exit(f"[x] 源文件缺失：{src}")
        shutil.copy2(src, target_dir / name)
    print(f"[+] 复制 exp_api.h / exp_api.cpp -> {target_dir}")

    print(f"[+] {patch_header(header)}")
    print("[i] 完成。库原文件只有 GxEPD2_290.h 被加了两行，其余未动。")


if __name__ == "__main__":
    main()
