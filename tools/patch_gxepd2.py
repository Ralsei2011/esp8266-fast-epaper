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


def patch_3c(lib: Path, check_only: bool) -> None:
    """三色屏 GxEPD2_290_C90c：整文件覆盖。

    黑白线（IL3820）只加两行 include + 宏就够了，但 C90c 这边新增了 7 个低层接口
    （setFastTemp/setUpdateMode/setGateLines/expActivate/expWriteLut/expSetRamArea/
    expWriteRam），还改了 _Update_Full/_Update_Part 的 0x22 下发逻辑，逐行注入不现实。
    driver/3c/ 里放的是基于 GxEPD2 1.6.9 改好的完整文件，直接覆盖。

    早期版本漏了这一步，导致 CI 上所有三色固件报：
        'class GxEPD2_290_C90c' has no member named 'setFastTemp'
    """
    src_dir = ROOT / "driver" / "3c"
    dst_dir = lib / "src" / "epd3c"
    names = ("GxEPD2_290_C90c.h", "GxEPD2_290_C90c.cpp")

    if not dst_dir.is_dir():
        print(f"[!] 跳过三色注入：{dst_dir} 不存在（这个 GxEPD2 版本可能没有 C90c）")
        return

    for name in names:
        if not (src_dir / name).is_file():
            sys.exit(f"[x] 三色驱动源文件缺失：{src_dir / name}")

    if check_only:
        text = (dst_dir / names[0]).read_text(encoding="utf-8")
        if "setFastTemp" not in text:
            print("[!] 三色驱动尚未注入")
            sys.exit(1)
        print("[=] 三色驱动已注入")
        return

    for name in names:
        shutil.copy2(src_dir / name, dst_dir / name)
    print(f"[+] 覆盖三色驱动 {len(names)} 个文件 -> {dst_dir}")

    # 覆盖完立刻验证，别让"看起来成功但没生效"再发生一次
    text = (dst_dir / names[0]).read_text(encoding="utf-8")
    missing = [m for m in ("setFastTemp", "setUpdateMode", "setGateLines",
                           "expActivate", "expSetRamArea", "expWriteRam")
               if m not in text]
    if missing:
        sys.exit(f"[x] 三色注入不完整，缺少：{missing}")


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

    # 三色屏先处理：它是本项目的主线，黑白屏那条是早期实验
    patch_3c(lib, args.check)

    text = header.read_text(encoding="utf-8")
    if MARK_INCLUDE in text and MARK_DECLS in text:
        print(f"[=] {lib} 黑白驱动已经注入过")
        return

    if args.check:
        print(f"[!] {lib} 黑白驱动尚未注入")
        sys.exit(1)

    for name in ("exp_api.h", "exp_api.cpp"):
        src = DRIVER / name
        if not src.is_file():
            sys.exit(f"[x] 源文件缺失：{src}")
        shutil.copy2(src, target_dir / name)
    print(f"[+] 复制 exp_api.h / exp_api.cpp -> {target_dir}")

    # 黑白驱动注入失败不该拖累三色固件：主线走的是 C90c，这条是早期 IL3820 实验用的
    try:
        print(f"[+] {patch_header(header)}")
    except SystemExit as e:
        print(f"[!] 黑白驱动注入跳过（不影响三色固件）：{e}")
    print("[i] 完成。")


if __name__ == "__main__":
    main()
