#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
把 Bad Apple 视频转成墨水屏帧数据。

屏幕: GATES=296 (水平) x SOURCES=128 (垂直)，横屏使用
画面: 170x128 居中，左右各 63px 黑边（保真模式）

输出两种体积对照：
  raw   每帧 296*16 = 4736 字节
  rle   逐行 RLE（每行 16 字节，用 0x00/0xFF 游程压缩）
  drle  与前帧做 XOR 后再 RLE（墨水屏只关心变化）

用法:
  python tools/mkbadapple.py --start 0 --dur 30 --out media/sample
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image
import imageio_ffmpeg

GATES = 296
SOURCES = 128
ROWB = SOURCES // 8  # 16


def ffmpeg_exe():
    return imageio_ffmpeg.get_ffmpeg_exe()


def iter_frames(video, start, dur, fps, vw, vh, pad_x):
    """让 ffmpeg 直接输出缩放+灰度 rawvideo，避免中间落盘。"""
    cmd = [
        ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-ss", str(start), "-t", str(dur), "-i", video,
        "-vf", f"fps={fps},scale={vw}:{vh}:flags=lanczos,pad={GATES}:{SOURCES}:{pad_x}:0:black,format=gray",
        "-f", "rawvideo", "-pix_fmt", "gray", "-",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=GATES * SOURCES * 4)
    fsize = GATES * SOURCES
    while True:
        buf = p.stdout.read(fsize)
        if not buf or len(buf) < fsize:
            break
        yield np.frombuffer(buf, dtype=np.uint8).reshape(SOURCES, GATES)
    p.stdout.close()
    p.wait()


def to_bits(gray, threshold=128):
    """gray: (SOURCES, GATES) 亮度；返回 (GATES, ROWB) 位图。
    亮度 < threshold 判为黑 -> bit 1（与固件 fb 语义一致：1=白底，0=黑）
    固件里 fbClear(0xFF) 是白，画黑用 v=0 清位。
    所以这里 黑像素 -> bit 0，白像素 -> bit 1。
    """
    on = (gray >= threshold)  # True = 白
    g = on.T  # (GATES, SOURCES)
    packed = np.packbits(g, axis=1, bitorder="big")
    return packed.astype(np.uint8)


def rle_pack(frame):
    """逐行 RLE。每行 16 字节，格式: [literal_count, bytes...] 与 [0x80|run_len, value] 交替。
    简化: 每条指令 1 字节头。低 7 位 = 长度(1..127)，高位 1 = 重复，0 = 字面量。
    """
    out = bytearray()
    flat = frame.reshape(GATES, ROWB)
    for row in flat:
        i = 0
        n = ROWB
        while i < n:
            # 找重复
            j = i
            while j + 1 < n and row[j + 1] == row[i] and (j - i) < 126:
                j += 1
            run = j - i + 1
            if run >= 3:
                out.append(0x80 | run)
                out.append(int(row[i]))
                i = j + 1
            else:
                # 字面量
                k = i
                while k < n and (k - i) < 127:
                    if k + 2 < n and row[k] == row[k + 1] == row[k + 2]:
                        break
                    k += 1
                lit = row[i:k]
                out.append(len(lit))
                out.extend(int(b) for b in lit)
                i = k
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", default="media/badapple.f30033.mp4")
    ap.add_argument("--start", type=float, default=0)
    ap.add_argument("--dur", type=float, default=30)
    ap.add_argument("--fps", type=float, default=20)
    ap.add_argument("--out", default="media/sample")
    ap.add_argument("--threshold", type=int, default=128)
    args = ap.parse_args()

    # 170x128 居中
    vh = SOURCES
    vw = int(round(512 / 384 * SOURCES))  # 170
    pad_x = (GATES - vw) // 2            # 63

    print(f"视频 {args.video}  {args.start}s + {args.dur}s @ {args.fps}fps")
    print(f"画面 {vw}x{vh} 居中于 {GATES}x{SOURCES}，左右各 {pad_x}px 黑边")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    raw_path = args.out + ".raw"
    rle_path = args.out + ".rle"
    drle_path = args.out + ".drle"

    n = 0
    raw_bytes = 0
    rle_bytes = 0
    drle_bytes = 0
    prev = None

    with open(raw_path, "wb") as fr, open(rle_path, "wb") as fq, open(drle_path, "wb") as fd:
        for gray in iter_frames(args.video, args.start, args.dur, args.fps, vw, vh, pad_x):
            fb = to_bits(gray, args.threshold)
            raw = fb.tobytes()
            fr.write(raw)
            raw_bytes += len(raw)

            q = rle_pack(fb)
            fq.write(q)
            rle_bytes += len(q)

            if prev is None:
                d = fb.copy()
            else:
                d = np.bitwise_xor(fb, prev)
            dq = rle_pack(d)
            fd.write(dq)
            drle_bytes += 1 + len(dq)  # 1 字节长度前缀（示意）
            drle_bytes += 0
            prev = fb
            n += 1

    # 存一张样张
    first = None
    for gray in iter_frames(args.video, args.start, 1.0 / max(args.fps, 1), args.fps, vw, vh, pad_x):
        first = gray
        break

    print()
    print(f"帧数        {n}")
    print(f"raw        {raw_bytes/1024/1024:8.2f} MB   ({raw_bytes/max(n,1):.0f} B/帧)")
    print(f"rle        {rle_bytes/1024/1024:8.2f} MB   ({rle_bytes/max(n,1):.0f} B/帧, {rle_bytes/max(raw_bytes,1)*100:.0f}%)")
    print(f"diff+rle   {drle_bytes/1024/1024:8.2f} MB   ({drle_bytes/max(n,1):.0f} B/帧, {drle_bytes/max(raw_bytes,1)*100:.0f}%)")
    if n:
        fps_trans = 921600 / 8 / (rle_bytes / n)
        print(f"\n若串口 921600 baud 传 rle: {fps_trans:.1f} fps 上限（不含刷新）")

    if first is not None:
        img = Image.fromarray(first)
        img.save(args.out + "_preview.png")
        print("样张:", args.out + "_preview.png")


if __name__ == "__main__":
    main()
