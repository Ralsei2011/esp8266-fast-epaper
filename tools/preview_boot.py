#!/usr/bin/env python3
"""
把 epd_vm 的启动画面按固件里同样的规则渲染成 PNG，烧录前先看排版。

固件是横屏绘制（逻辑 296x128），物理屏是 128x296 竖排，所以输出两张图：
  preview_boot.png      横屏逻辑画面，你横着拿屏时看到的样子
  preview_boot_phys.png 旋转 90° 后的物理帧缓冲，用来核对旋转方向对不对

行格式与固件一致：<字号>|<文本>，字号 1~4，# 开头是注释。
占位符用假值填充：%V %F %S %H %D %P

用法：
    python tools/preview_boot.py                     # 用内置默认文本
    python tools/preview_boot.py --from-flash dump.txt   # 用你打算 PUT 的文本
"""
import argparse
import pathlib
import re
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent

LW, LH = 296, 128          # 横屏逻辑尺寸
PW, PH = 128, 296          # 物理屏尺寸

# 与固件 BOOT_DEFAULT_TXT 保持一致
DEFAULT_TXT = """\
2|EPD VM  %V
1|%F MHz   %S MB flash   heap %H
1|driver  %D
1|panel   %P
2|BOOT OK
"""

# 占位符的假值，仅用于预览
FAKE = {
    "V": "1.1",
    "F": "80",
    "S": "16",
    "H": "14240",
    "D": "GxEPD2_290_C90c",
    "P": "E029A10N 128x296",
}


def load_font():
    cand = list(ROOT.glob("ardata/user/libraries/Adafruit_GFX_Library/glcdfont.c"))
    if not cand:
        sys.exit("[x] 找不到 glcdfont.c")
    src = cand[0].read_text(encoding="utf-8", errors="replace")
    body = src[src.index("{"):]
    vals = [int(x, 16) for x in re.findall(r"0[xX]([0-9A-Fa-f]{2})", body)]
    return vals[: 256 * 5]


def expand(text):
    out = []
    i = 0
    while i < len(text):
        if text[i] == "%" and i + 1 < len(text):
            key = text[i + 1].upper()
            if key in FAKE:
                out.append(FAKE[key])
                i += 2
                continue
        out.append(text[i])
        i += 1
    return "".join(out)


def parse_lines(raw):
    rows = []
    for line in raw.splitlines():
        if not line or line.startswith("#"):
            continue
        sz = 2
        txt = line
        if len(line) > 2 and line[0] in "1234" and line[1] == "|":
            sz = int(line[0])
            txt = line[2:]
        if txt:
            rows.append((expand(txt), sz))
    return rows[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from-flash", default="", help="要 PUT 进 boot 的文本文件")
    ap.add_argument("--scale", type=int, default=3)
    args = ap.parse_args()

    raw = (pathlib.Path(args.from_flash).read_text(encoding="utf-8")
           if args.from_flash else DEFAULT_TXT)
    rows = parse_lines(raw)
    font = load_font()

    logical = Image.new("L", (LW, LH), 255)
    lp = logical.load()

    def draw_char(x, y, ch, sz):
        base = ord(ch) * 5
        for i in range(5):
            line = font[base + i]
            for j in range(8):
                if line & 1:
                    for dy in range(sz):
                        for dx in range(sz):
                            xx, yy = x + i * sz + dx, y + j * sz + dy
                            if 0 <= xx < LW and 0 <= yy < LH:
                                lp[xx, yy] = 0
                line >>= 1

    total = sum(8 * sz + 4 for _, sz in rows)
    y = max(2, (LH - total) // 2)
    warn = []
    for text, sz in rows:
        w = len(text) * 6 * sz
        x0 = (LW - w) // 2
        if w > LW:
            warn.append(f"{text!r} 宽 {w} > {LW}")
        if y + 8 * sz > LH:
            warn.append(f"{text!r} 下边界 {y + 8 * sz} > {LH}")
        for k, ch in enumerate(text):
            draw_char(x0 + k * 6 * sz, y, ch, sz)
        y += 8 * sz + 4

    # 逻辑 -> 物理：顺时针 90°，(lx,ly) -> (px=ly, py=PH-1-lx)
    phys = Image.new("L", (PW, PH), 255)
    pp = phys.load()
    for lx in range(LW):
        for ly in range(LH):
            pp[ly, PH - 1 - lx] = lp[lx, ly]

    out1 = ROOT / "preview_boot.png"
    out2 = ROOT / "preview_boot_phys.png"
    logical.resize((LW * args.scale, LH * args.scale), Image.NEAREST).save(out1)
    phys.resize((PW * args.scale, PH * args.scale), Image.NEAREST).save(out2)
    print(f"[+] {out1}  横屏视角 {LW}x{LH}")
    print(f"[+] {out2}  物理帧缓冲 {PW}x{PH}")
    print(f"[+] {len(rows)} 行，总高 {total}px")
    if warn:
        print("[!] 排版问题：")
        for w in warn:
            print("   ", w)
    else:
        print("[+] 全部文字在画布内")


if __name__ == "__main__":
    main()
