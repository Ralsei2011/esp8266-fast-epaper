#!/usr/bin/env python3
"""
只发 RUN，不 PUT —— 脚本已经在 flash 里时用这个，避免反复擦写同一扇区。

用法：
    python -u tools/run_only.py w8 --panel 1
    python -u tools/run_only.py w8 --panel 2 --dry   # 只 LS 列目录

结果写 results/vm_matrix_panel<N>.csv
"""
import argparse
import pathlib
import re
import sys
import time

import serial
import serial.tools.list_ports as lp

ROOT = pathlib.Path(__file__).resolve().parent.parent


def find_port():
    for p in lp.comports():
        if "CH340" in (p.description or "").upper():
            return p.device
    ports = [p.device for p in lp.comports()]
    if ports:
        return ports[0]
    sys.exit("[x] 找不到串口")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--port", default="")
    ap.add_argument("--panel", default="?")
    ap.add_argument("--dry", action="store_true", help="只列目录，不执行")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"[+] {port}  RUN {args.name}  panel {args.panel}")

    s = serial.Serial(port, 2000000, timeout=0.5)
    s.reset_input_buffer()
    time.sleep(1.0)
    s.write(b"\n")
    s.flush()
    time.sleep(0.4)
    s.read(2000)

    def talk(cmd, wait_for, timeout, tail=0.0):
        s.write((cmd + "\n").encode())
        s.flush()
        t0 = time.time()
        buf = b""
        hit = 0.0
        while time.time() - t0 < timeout:
            d = s.read(300)
            if d:
                buf += d
                if hit == 0.0 and wait_for in buf:
                    hit = time.time()
                    if tail <= 0:
                        break
            if hit and time.time() - hit > tail:
                break
        return buf.decode(errors="replace")

    # LS 先回 "OK LS n"，再逐条列目录，所以要等尾部数据
    ls = talk("LS", b"OK LS", 10, tail=0.8)
    print("[i] 目录：")
    for line in ls.splitlines():
        if line.strip() and "expActivate" not in line:
            print("   ", line)
    if "OK LS" not in ls:
        sys.exit("[x] 固件没响应，可能还没上电或没走完开机流程（等 READY）")
    if args.name not in ls:
        sys.exit(f"[x] flash 里没有 {args.name}，需要先 PUT")
    if args.dry:
        s.close()
        return

    print("[i] RUN，全程约 110 秒，请盯屏...")
    t0 = time.time()
    buf = b""
    s.write((f"RUN {args.name}\n").encode())
    s.flush()
    while time.time() - t0 < 240:
        d = s.read(400)
        if d:
            buf += d
            txt = buf.decode(errors="replace")
            if "DONE " in txt or "ERR run" in txt or "ERR none" in txt:
                break
    s.close()

    out = buf.decode(errors="replace")
    for line in out.splitlines():
        if line.strip() and "expActivate" not in line:
            print("   ", line)

    trigs = re.findall(r"TRIG 0x([0-9A-Fa-f]{2}) (\d+) ms", out)
    if trigs:
        # 数据体检：按步骤号区分整屏/半程（不能按时长猜，2948ms 是正常半程值）。
        # 这批屏 0xF7 整屏 4.8~5.4 秒，GDR148 半程 2.7~3.1 秒。
        # 明显偏短 = BUSY 线接触不良，waitBusy 提前返回，读数不可信。
        FULL_STEPS = {1, 2, 3, 4, 7, 8, 9}
        bad = []
        for i, (_, ms) in enumerate(trigs, 1):
            v = int(ms)
            if i in FULL_STEPS and v < 4600:
                bad.append(v)
            elif i not in FULL_STEPS and v < 2600:
                bad.append(v)
        if bad:
            print(f"\n[!] 可疑读数 {bad} ms —— 明显短于该型号正常值")
            print("    BUSY 线接触不良会让等待提前返回，屏幕本身没变快。")
            print("    请断电后重插排线再测；本次数据不写入 results/")
            path = ROOT / "results" / "rejected"
            path.mkdir(exist_ok=True)
            out_path = path / f"vm_matrix_panel{args.panel}_suspect.csv"
        else:
            path = ROOT / "results"
            out_path = path / f"vm_matrix_panel{args.panel}.csv"
        with open(out_path, "w", encoding="utf-8") as f:
            f.write("step,mode,ms\n")
            for i, (m, ms) in enumerate(trigs, 1):
                f.write(f"{i},{m},{ms}\n")
        print(f"[+] {len(trigs)} 次刷新 -> {out_path}")
        print("    " + "  ".join(ms for _, ms in trigs))


if __name__ == "__main__":
    main()
