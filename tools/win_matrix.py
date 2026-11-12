#!/usr/bin/env python3
"""
区域刷矩阵采集器 —— 配合 epd_3c_win 固件
==========================================================================
跑一次固件的 S 命令，把每一步的「起始时刻 / 耗时」记成 CSV，
方便五块屏横向对比，也方便事后按时间戳回看"那一刻屏幕上该是什么"。

用法:
    python tools/win_matrix.py --panel 5
    python tools/win_matrix.py --panel 5 --port COM62 --out results/win_panel5.csv

输出列：
    panel, step, start_s, dur_ms, expect
其中 start_s 是从发出 S 命令算起的秒数 —— 你按这个时间回看屏幕录像即可。
"""
import argparse
import csv
import re
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports as lp

ROOT = Path(__file__).resolve().parent.parent

# 每一步"应该看到什么"，写进 CSV 方便对照
EXPECT = {
    "S1_FULL_BLACK":        "整屏黑",
    "S2_BOTTOM_WHITE_y148": "下半屏变白，上半屏保持黑",
    "S3_FULL_BLACK":        "整屏黑",
    "S4_LEFT_WHITE_x0w64":  "左半边（宽 64px）变白，右半保持黑",
    "S5_FULL_BLACK":        "整屏黑",
    "S6_GDR148_BOTTOM_y148": "预期：无变化（扫描只到 gate147）",
    "S7_GDR148_TOP_y0":     "上半屏变白",
    "S8_FULL_WHITE":        "整屏白",
}

STEP_RE = re.compile(r"STEP (\S+) START")
DONE_RE = re.compile(r"STEP (\S+) DONE (\d+) ms")


def find_port(explicit=None):
    if explicit:
        return explicit
    for p in lp.comports():
        desc = (p.description or "").upper()
        if "CH340" in desc or "CP210" in desc:
            return p.device
    sys.exit("[x] 没找到串口，检查板子是否插好")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--panel", default="?", help="屏编号，写进 CSV")
    ap.add_argument("--port", default=None, help="默认自动找 CH340")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--out", default="", help="CSV 路径")
    ap.add_argument("--log", default="", help="原始串口日志路径")
    args = ap.parse_args()

    port = find_port(args.port)
    out = Path(args.out) if args.out else ROOT / "results" / f"win_panel{args.panel}.csv"
    out.parent.mkdir(parents=True, exist_ok=True)
    log = Path(args.log) if args.log else out.with_suffix(".log")

    ser = serial.Serial(port, args.baud, timeout=1)
    print(f"[+] {port} @{args.baud}")
    time.sleep(1.2)
    ser.reset_input_buffer()
    # 打开串口后第一条命令的首字节必定被吃掉，先发空行垫一下
    ser.write(b"\n")
    ser.flush()
    time.sleep(0.4)

    t0 = time.time()
    ser.write(b"S\n")
    ser.flush()
    print("[+] 已发出 S，矩阵开跑")

    rows = []
    cur = None
    raw = open(log, "w", encoding="utf-8")

    while True:
        line = ser.readline()
        if not line:
            if time.time() - t0 > 200:
                print("[!] 超时退出")
                break
            continue

        text = line.decode(errors="replace").strip()
        if not text or "expActivate" in text:
            continue

        el = time.time() - t0
        raw.write(f"{el:7.1f}s  {text}\n")
        raw.flush()

        m = STEP_RE.search(text)
        if m:
            cur = m.group(1)
            print(f"{el:6.1f}s  >>> {cur:24s} {EXPECT.get(cur, '')}")
            continue

        m = DONE_RE.search(text)
        if m:
            step, dur = m.group(1), int(m.group(2))
            rows.append({
                "panel": args.panel,
                "step": step,
                "start_s": round(el - dur / 1000.0, 1),
                "dur_ms": dur,
                "expect": EXPECT.get(step, ""),
            })
            print(f"{el:6.1f}s      {step:24s} {dur} ms")
            continue

        if "MATRIX END" in text:
            print(f"{el:6.1f}s  ==== 结束 ====")
            break

    ser.close()
    raw.close()

    if not rows:
        sys.exit("[x] 没收到任何 STEP 数据，检查固件是否为 epd_3c_win")

    with open(out, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=["panel", "step", "start_s", "dur_ms", "expect"])
        w.writeheader()
        w.writerows(rows)

    print(f"\n[+] {len(rows)} 步写入 {out}")
    print(f"[+] 原始日志 {log}")
    print("\n按时间回看屏幕：")
    for r in rows:
        print(f"   {r['start_s']:6.1f}s  {r['step']:24s} {r['dur_ms']:>6} ms   {r['expect']}")


if __name__ == "__main__":
    main()
