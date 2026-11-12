# -*- coding: utf-8 -*-
"""
生成 20px 粗体中文字库（C 头文件）

背景：wqy16 点阵字笔画只有 1px、字内间隙 1px，任何"叠加式加粗"都会把笔画糊在一起。
      正确做法是不加粗，直接用天生就粗的矢量字体渲染成位图。

用法：
    python tools/gen_cn_font.py
    EPD_FONT=D:/fonts/xxx.ttc python tools/gen_cn_font.py    # 换字体

默认字体是 Windows 自带的微软雅黑 Bold。**生成出的 cn_font.h 不入库** ——
它是字体厂商的衍生位图，版权归字体厂商。

输出格式：
  - 高度固定 24px（基线在 y=20）
  - 中文宽 20px，半角宽 10px
  - 每行 3 字节（24 bit），行优先，bit7 为最左像素
  - 索引按 Unicode 升序，固件里二分查找
"""
import os, re, glob, sys
from PIL import Image, ImageFont, ImageDraw

FONT_PATH = os.environ.get("EPD_FONT", r"C:\Windows\Fonts\msyhbd.ttc")   # 微软雅黑 Bold
FONT_SIZE = 20
W_FULL = 20      # 全角宽
W_HALF = 10      # 半角宽
PROBE_Y = 40     # 探测用的临时基线（足够低，不会被裁）
BYTES_PER_ROW = 3

_HERE = os.path.dirname(os.path.abspath(__file__))
OUT_H = os.path.normpath(os.path.join(_HERE, "..", "research", "3color",
                                      "epd_font_test", "cn_font.h"))

# ---------- 1. 收集字符 ----------
chars = set()

# 1a. 从现有固件源码里提取
for f in glob.glob(os.path.join("epd_ui_demo", "*.ino")):
    s = open(f, encoding="utf-8").read()
    for m in re.findall(r'"([^"]*)"', s):
        for ch in m:
            if ord(ch) > 127:
                chars.add(ch)

# 1b. 常用汉字（一级常用字按频率），将来加名言/天气/节日不缺字
COMMON = (
    "的一是了我不人在他有这个上们来到时大地为子中你说生国年着就那和要她出也得里后自以会"
    "家可下而过天去能对小多然于心学么之都好看起发当没成只如事把还用第样道想作种开美总从"
    "无情己面最女但现前些所同日手又行意动方期它头经长儿回位分爱老因很给名法间斯知世什两"
    "次使身者被高已亲其进此话常与活正感见明问力理尔点文几定本公特做外孩相西果走将月十实"
    "向声车全信重三机工物气每并别真打太新比才便夫再书部水像眼等体却加电主界门利海受听表"
    "德少克代员许稜先口由死安写性马光白或住难望教命花结乐色更拉东神记处让母父应直字场平"
    "报友关放至张认接告入笑内英军候民岁往何度山觉路带万男边风解叫任金快原吃妈变通师立象"
    "数四失满战远格士音轻目条呢病始达深完今提求清王化空业思切怎非找片罗钱紶吗语元喜曾离"
    "飞科言干流欢约各即指合反题必该论交终林请医晚制球决窢传画保读运及则房早院量苦火布品"
    "近坐产答星精视五连司巴奇管类未朋且婚台夜青北队久乎越观落尽形影红爸百令周吧识步希亚"
    "术留市半热送兴造谈容极随演收首根讲整式取照办强石古华諣拿计您装似足双妻尼转诉米称丽"
    "客南领节衣站黑刻统断福城故历惊脸选包紧争另建维绝树系伤示愿持千史谁准联妇纪基买志静"
    "阿诗独复痛消社算义竟确酒需单治卡幸兰念举仅钟怕共毛句息功官待究跟穿室易游程号居考突"
    "皮哪费倒价图具刚脑永歌响商礼细专黄块脚味灵改据般破引食仍存众注笔甚某沉血备习校默务"
    "土微娘须试怀料调广苏显赛查密议底列富梦错座参八除跑亮假印设线温虽掉京初养香停际致阳"
    "纸李纳验助激够严证帝饭忘趣支春集丈木研班普导顿睡展跳获艺六波察群皇段急庭创区奥器谢"
    "弟店否害草排背止组州朝封睛板角况曲馆育忙质河续哥呼若推境遇雨标姐充围案伦护冷警贝著"
    "雪索剧啊船险烟依斗值帮汉慢佛肯闻唱沙局伯族低玩资屋击速顾泪洲团圣旁堂兵七露园牛哭旅"
    "街劳型烈姑陈莫鱼异抱宝权鲁简态级票怪寻杀律胜份汽右洋范床舞秘午登楼贵吸责例追较职属"
    "渐左录丝牙党继托赶章智冲叶胡吉卖坚喝肉遗救修松临富辆宪 prepay 车"
)
for ch in COMMON:
    if '\u4e00' <= ch <= '\u9fff':
        chars.add(ch)

# 1c. 标点与符号
for ch in "℃°%→。，、：；！？（）《》“”‘’—…·":
    chars.add(ch)

chars = sorted(chars)

# ---------- 2. 渲染 ----------
try:
    font = ImageFont.truetype(FONT_PATH, FONT_SIZE, index=0)
except Exception as e:
    print("字体加载失败:", e)
    sys.exit(1)

ascent, descent = font.getmetrics()
print(f"字体 ascent={ascent} descent={descent}")

# ---------- 2a. 先探测所有字的真实墨迹范围 ----------
PROBE_H = 80
min_top, max_bot = PROBE_H, 0
for ch in chars:
    is_full = not (ch.isascii() and ord(ch) < 128)
    w = W_FULL if is_full else W_HALF
    img = Image.new("1", (w, PROBE_H), 0)
    d = ImageDraw.Draw(img)
    d.text((0, PROBE_Y), ch, font=font, fill=1, anchor="ls")
    bbox = img.getbbox()          # (l, t, r, b) 或 None
    if bbox:
        min_top = min(min_top, bbox[1])
        max_bot = max(max_bot, bbox[3])

ink_above = PROBE_Y - min_top      # 基线以上多少像素
ink_below = max_bot - PROBE_Y      # 基线以下多少像素
H = ink_above + ink_below          # 恰好容纳全部墨迹
BASE_Y = ink_above                 # 基线位置
print(f"墨迹范围: 基线上 {ink_above}px / 基线下 {ink_below}px -> 画布高 {H}px, 基线 y={BASE_Y}")
if H > 28:
    print("!! 字太高，建议减小 FONT_SIZE")
    sys.exit(1)

# ---------- 2b. 正式渲染 ----------
bits = bytearray()
index = []
missing = []
index = []
missing = []

for ch in chars:
    is_full = not (ch.isascii() and ord(ch) < 128)
    w = W_FULL if is_full else W_HALF

    img = Image.new("1", (w, H), 0)
    d = ImageDraw.Draw(img)
    try:
        d.text((0, BASE_Y), ch, font=font, fill=1, anchor="ls")
    except Exception:
        d.text((0, BASE_Y - ascent), ch, font=font, fill=1)

    px = img.load()
    blank = True
    row_bytes = bytearray()
    for y in range(H):
        b = 0
        for x in range(w):
            if px[x, y]:
                b |= (1 << (7 - (x % 8)))
                blank = False
            if (x % 8) == 7:
                row_bytes.append(b)
                b = 0
        # 补齐到 BYTES_PER_ROW
        if w % 8:
            row_bytes.append(b)
        while len(row_bytes) % BYTES_PER_ROW:
            row_bytes.append(0)
    if blank:
        missing.append(ch)

    off = len(bits)
    bits.extend(row_bytes)
    index.append((ord(ch), w, off))

# 补齐对齐
while len(bits) % 4:
    bits.append(0)

print(f"字符数={len(chars)}  位图字节={len(bits)}  缺字形={len(missing)}")
if missing:
    print("  缺字形字符:", "".join(missing))

# ---------- 3. 输出 ----------
with open(OUT_H, "w", encoding="utf-8") as f:
    f.write("// 自动生成：20px 微软雅黑 Bold 点阵字库\n")
    f.write(f"// 字高 {H}px（基线 {BASE_Y}），中文宽 {W_FULL}px，半角宽 {W_HALF}px\n")
    f.write(f"// 字符数 {len(chars)}，位图 {len(bits)} 字节\n")
    f.write("// 索引按 Unicode 升序，固件二分查找\n")
    f.write("// 每个字固定 CHARSIZE 字节（行优先 3 字节/行），偏移 = 序号 * CHARSIZE\n")
    f.write("#pragma once\n#include <stdint.h>\n")
    f.write("#include <pgmspace.h>\n\n")
    f.write(f"#define CNF_H {H}\n#define CNF_BASE {BASE_Y}\n")
    f.write(f"#define CNF_ROWBYTES {BYTES_PER_ROW}\n")
    f.write(f"#define CNF_CHARSIZE (CNF_H * CNF_ROWBYTES)\n")
    f.write(f"#define CNF_COUNT {len(chars)}\n\n")

    f.write("typedef struct {\n  uint16_t code;\n  uint8_t  w;\n} CnGlyph;\n\n")

    f.write("static const uint8_t CNF_BITS[] PROGMEM = {\n")
    for i in range(0, len(bits), 24):
        f.write("  " + ",".join(str(b) for b in bits[i:i+24]) + ",\n")
    f.write("};\n\n")

    f.write("static const CnGlyph CNF_IDX[] PROGMEM = {\n")
    for code, w, off in index:
        idx = off // (H * BYTES_PER_ROW)
        ch = chr(code)
        hint = ch if ch.isprintable() and ch != '"' and ch != '\\' else '?'
        f.write(f"  {{0x{code:04X},{w}}}, // {hint}\n")
    f.write("};\n")

sz = os.path.getsize(OUT_H)
print(f"输出 {OUT_H}  大小 {sz/1024:.1f} KB")
