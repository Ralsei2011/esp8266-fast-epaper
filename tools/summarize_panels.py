#!/usr/bin/env python3
"""
汇总 results/vm_matrix_panel*.csv，出跨屏横向对比表。

用法：
    python -u tools/summarize_panels.py
    python -u tools/summarize_panels.py --model   # 顺带拟合“扫描行数 -> 耗时”模型
"""
import argparse
import csv
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
RES = ROOT / "results"

STEP_DESC = {
    1: "整屏黑 GDR296",
    2: "下半窗口 y=148",
    3: "上半窗口推黑",
    4: "左竖带 x0 w64",
    5: "GDR148+下半窗",
    6: "GDR148+上半窗",
    7: "整屏推黑",
    8: "中心块 64x64",
    9: "收尾整屏白",
}


def load():
    panels = {}
    for p in sorted(RES.glob("vm_matrix_panel*.csv"), key=lambda x: x.name):
        m = re.search(r"panel(\w+)\.csv$", p.name)
        if not m:
            continue
        rows = []
        with open(p, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                rows.append((int(r["step"]), r["mode"], int(r["ms"])))
        if rows:
            panels[m.group(1)] = rows
    return panels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", action="store_true")
    args = ap.parse_args()

    panels = load()
    if not panels:
        print("[x] results/ 里还没有 vm_matrix_panel*.csv")
        return

    keys = list(panels.keys())
    nstep = max(len(v) for v in panels.values())

    w = max(18, max(len(v) for v in STEP_DESC.values()) + 2)
    head = f"{'步':<3}{'动作':<{w}}" + "".join(f"{'屏' + k:>9}" for k in keys)
    if len(keys) > 1:
        head += f"{'极差':>8}"
    print(head)
    print("-" * len(head))

    for i in range(1, nstep + 1):
        vals = []
        for k in keys:
            row = next((r for r in panels[k] if r[0] == i), None)
            vals.append(row[2] if row else None)
        line = f"{i:<3}{STEP_DESC.get(i, '?'):<{w}}"
        for v in vals:
            line += f"{v if v is not None else '-':>9}"
        if len(keys) > 1:
            known = [v for v in vals if v]
            line += f"{(max(known) - min(known)) if len(known) > 1 else 0:>8}"
        print(line)

    # 均值 / 一致性
    print("-" * len(head))
    for i in (5, 6):
        vals = [next((r[2] for r in panels[k] if r[0] == i), None) for k in keys]
        known = [v for v in vals if v]
        if len(known) > 1 and known[0]:
            ratios = [
                next((r[2] for r in panels[k] if r[0] == i), 0)
                / next((r[2] for r in panels[k] if r[0] == 1), 1)
                for k in keys
            ]
            print(f"    S{i}/S1 半程比：" + "  ".join(f"{x:.3f}" for x in ratios)
                  + f"   (均值 {sum(ratios)/len(ratios):.3f})")

    if args.model:
        print("\n[模型] t = a + b * rows   （用 S1=296 行、S5=148 行两点解）")
        for k in keys:
            t1 = next((r[2] for r in panels[k] if r[0] == 1), None)
            t5 = next((r[2] for r in panels[k] if r[0] == 5), None)
            if not (t1 and t5):
                continue
            b = (t1 - t5) / (296 - 148)
            a = t1 - b * 296
            print(f"    屏{k}: a={a:7.1f} ms  b={b:6.2f} ms/行"
                  f"   -> 预测 GDR=148: {a + b*148:6.1f} (实测 {t5})")


if __name__ == "__main__":
    main()
