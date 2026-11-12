# IL3820 墨水屏快刷实验：把 2.9 寸电子纸跑到 30fps

一块 2.9 寸 128×296 黑白电子纸（**IL3820** 控制器），出厂规格是全刷 1.65 秒、局刷 0.41 秒。
本项目把它压到 **单帧 9.18 ms**，并在上面**以 30fps 连续播放 Bad Apple**。

| | 出厂 | 本项目 | 提升 |
|---|---|---|---|
| 单帧刷新 | 412 ms（partial） | **9.18 ms** | **45×** |
| 等效帧率 | 2.4 fps | **109 fps**（驱动能力） | 45× |
| 实际播放 | — | **30 fps** | — |

> 这是一个纯实验项目。所有数字都是在这块具体的板子上实测出来的，不是手册标称值。
> 数据、失败路径、以及被推翻的结论都记录在 [`docs/`](docs/)。

---

## 目录

- [硬件](#硬件)
- [四个关键结论](#四个关键结论)
- [仓库结构](#仓库结构)
- [快速开始](#快速开始)
- [固件一：epd_ctl（脚本化探测台）](#固件一epd_ctl脚本化探测台)
- [固件二：epd_play（播放器）](#固件二epd_play播放器)
- [性能实测数据](#性能实测数据)
- [实验记录](#实验记录)
- [已知限制](#已知限制)
- [关于版权](#关于版权)

---

## 硬件

| 部件 | 型号 / 参数 |
|---|---|
| 电子纸 | 2.9 寸，128×296，黑白，IL3820 控制器（兼容 SSD1608） |
| 主控 | ESP8266（NodeMCU 类板，4MB flash） |
| 接线 | CS=GPIO15, DC=GPIO0, RST=GPIO2, BUSY=GPIO4 |
| 串口 | CH340，2 Mbaud |

屏幕的像素组织是 **296 gate（行）× 128 source（列）**，横屏使用。
帧缓冲 296×16 = **4736 字节**（1bpp）。

---

## 四个关键结论

### 1. 波形是可以从外部写进去的

网上常见的说法是"IL3820 的波形固化在 OTP 里，改不了"。**实测可以改** —— 通过 `0x32` 写 30 字节 LUT 寄存器，
刷新时长会严格跟着 LUT 里的时间片总和走。之前认为"改不了"，是因为 `0x22 = 0x10`（Load LUT）
会**用 OTP 里的波形覆盖掉你刚写进去的 LUT**，看起来就像写入无效。

### 2. 帧时间不是常数，而是 `MUX × TGate`

这是整件事成立的基础。实测：

```
T_FRAME = (MUX + DummyLine) × T_GATE
T_GATE  = 31 µs        （0x3B = 0x00 时；出厂默认约 62 µs）
单帧时长 = sumTP × T_FRAME
```

出厂之所以慢，是因为它用满 296 行 × 20 个时间片。把 `sumTP` 降到 1，单帧就是
`296 × 31µs = 9.18ms`。MUX 从 296 一路砍到 16，帧时间严格线性降到 497µs，验证了这个模型。

### 3. `0x10` 只能单向驱动，必须用 `0x18`

LUT 里每个 VS 字节描述 4 种像素转变各自的电平（2 bit：`00`=GND，`01`=+VSH，`10`=−VSL），
出厂 partial 波形的前几相解码如下：

| 字节 | 四个 2bit 字段 | 含义 |
|---|---|---|
| `0x10` | `[0,0,1,0]` | 纯单向：只把白推成黑，黑推不回白 |
| `0x18` | `[0,2,1,0]` | 双向：一个 −VSL、一个 +VSH |
| `0x08` | `[0,2,0,0]` | 反向单向 |

只取 `0x10` 做快刷，会出现两个典型症状：**动的东西墨色很淡**（每个位置只被推一次）、
**移走的残影擦不掉，最后累积成反相的白斑**。换成 `0x18` 后残影立刻消失。

### 4. 真正的天花板是供电，不是芯片

驱动电流随"同一时刻有多少条 source 线在放电"上升。实测边界：

- 单帧驱动 **≤ 9.18ms**（sumTP=1）：任意图案都能长时间跑，30fps 稳定
- 单帧驱动 **≥ 18.4ms**（sumTP=2）：只有小图案能活，大面积图案必掉电
- **持续驱动 > 300ms**：不管平均占空比多低，都会触发 USB 口过流保护 → 板子掉电复位

掉电时 `getResetReason()` 返回 `Power On` 且 RTC 用户内存丢失 —— 是真的断电，不是崩溃或看门狗。

---

## 仓库结构

```
firmware/
  epd_ctl/         脚本化探测台固件：烧一次，之后所有实验靠串口文本命令
  epd_play/        播放器固件：收 RLE 帧包 → 解码 → 上传 → 刷新
driver/
  exp_api.h        给 GxEPD2_290 追加的低层寄存器接口（声明宏 + ExpTiming）
  exp_api.cpp      对应实现
tools/
  build.py         一键编译（arduino-cli 封装）
  flash.py         自动找 CH340 端口并烧录
  epd.py           epd_ctl 的脚本驱动器（跑 scripts/*.epd，全程落日志）
  probe_run.py     烧录 + 全程抓串口日志
  badapple.py      prep（视频转 RLE 帧序列）/ play（推流 + 网页预览）
  mkbadapple.py    早期版本的单文件转换器
scripts/           t00..t25 共 26 个实验脚本，一个脚本对应一个问题
docs/
  waveform.md      波形寄存器逐位说明
  power-budget.md  电流预算与掉电定位过程
  experiments.md   26 个实验的目的与结论
```

---

## 快速开始

### 依赖

```bash
pip install pyserial numpy pillow imageio-ffmpeg
```

还需要：

- [arduino-cli](https://arduino.github.io/arduino-cli/) 放到 `tools/arduino-cli.exe`
- ESP8266 core 3.1.2（`arduino-cli core install esp8266:esp8266 --additional-urls https://arduino.me/packages/esp8266.json`）
- [GxEPD2](https://github.com/ZinggJM/GxEPD2) 库
- `tools/cli-config.yaml`：复制 `tools/cli-config.example.yaml` 并改成你自己的路径

### 装上驱动扩展

把 `driver/exp_api.h`、`driver/exp_api.cpp` 拷进 `GxEPD2/src/epd/`，然后在 `GxEPD2_290.h` 里加两行：

```cpp
#include "exp_api.h"        // 放在 class 声明之前
class GxEPD2_290 : public GxEPD2_EPD
{
public:
  EPD_EXP_DECLS             // 放在 public 区
```

库原有代码一行都不用改。

### 编译与烧录

```bash
python tools/build.py epd_ctl              # 编译
python tools/build.py epd_ctl --flash      # 编译并烧录
python tools/build.py epd_ctl --clean      # 改了源码却像没生效时用这个
```

产物在 `build/<sketch>_default_out/<sketch>.ino.bin`。

> **注意**：arduino-cli 的增量缓存有时不拾取改动，烧完看着像"新固件没生效"。
> 这个坑踩过多次，所以给 `build.py` 加了 `--clean`。

### 播放

```bash
# 1. 把视频转成帧序列（不放仓库里，见「关于版权」）
python tools/badapple.py prep --video media/badapple.mp4 --fps 30

# 2. 推流播放
python tools/badapple.py play --file media/badapple_30fps.rle \
       --tp 2 --wash 600 --preview
```

`--preview` 会在 `http://127.0.0.1:8777` 开一个网页，3 倍放大实时显示屏上画面，
并给出帧号与实时 fps。PNG 编码跑在独立线程，不拖慢播放。

---

## 固件一：epd_ctl（脚本化探测台）

做这个固件的原因很实际：**每改一个参数就重编译 60 秒，实验做不下去。**
现在只烧一次，之后所有实验都是串口文本命令。

```
ver                      打印版本
clr <0|1>                清缓冲（1=白 0=黑）
rect <g0> <g1> <s0> <s1> <v>
disc <cx> <cy> <r> <v>   实心圆
bar <idx> [w]            竖条
up [fast|slow|both]      上传显存，回 U,<us>,<mode>
lut wave <ph> <dur>      用出厂 7 个驱动相生成波形
lut vs <vs_hex> <tp> ... 直接指定每一相的 VS 字节与时间片（核心命令）
lut hex <60位hex>        直接写 30 字节
lut full | lut part      出厂全刷 / 局刷波形
gate <g> <d>             0x3B TGate / 0x3A DummyLine
mux <n>                  0x01 有效门线数
temp <摄氏度>             0x1A 温度寄存器
cp | clk | off           0x22 = C0 / 80 / 03
mode <hex> [to_ms]       任意 0x22 值触发并计时
scan [to_ms]             等价 mode 04
anim <ph> <dur> <frames> [gap] [fstart] [draw] [both] [s]
wash [n]                 刷白（先 hibernate 再 clearScreen）
blink <n> [gap]          全屏黑白翻转
bb                       打印黑盒状态
```

`anim` 的 `draw` 是位掩码，用来做图案的剂量-反应实验：
`1`=全宽横条 `2`=弹跳圆 `4`=半宽横条 `8`=单行横条 `16`=竖条。

**黑盒**：每次启动把复位原因、上次跑到第几帧写进 EEPROM，同时用 RTC 用户内存做存活标记。
RTC 内容保留 = 软件复位；RTC 内容丢失 = 真的掉电。这是区分"崩溃"和"断电"最可靠的手段。

---

## 固件二：epd_play（播放器）

```
协议：0xA5 0x5A <type> <payload>

'F' len_lo len_hi <RLE>   上传并显示一帧
'W'                       快速全刷清残影
'T' <tp>                  设置时间片数（1..15），立即生效
'N' <lo> <hi>             设置自动全刷间隔（帧）
'Q'                       查询状态

应答：'R' 刷新完成    'W' 全刷完成    'E' 出错
```

一帧的数据流：RLE 解压 → `0x24` 写入显存 → `0x22=0x04` 触发扫描 → 回 `'R'`。
升压只在初始化时开一次，之后每帧都跳过 power on/off（每次省约 180ms）。

RLE 格式很朴素：`0x80|count` + 单字节（重复），或 `count` + 原样字节（字面量），`count=0` 表示 128。
1216 字节/帧，约为原始 4736 字节的 26%。

---

## 性能实测数据

固定 MUX=296、TGate=0x00，2 Mbaud（1216 字节传输 ≈ 4.9ms）：

| TP | 单帧刷新 | 实测帧率 | 墨色 | 长跑稳定性 |
|---|---|---|---|---|
| 6 | 56.4 ms | 13.0 fps | 最深 | 208 秒 |
| 3 | 28.9 ms | 25.2 fps | 中 | 稳定 |
| **2** | **19.8 ms** | **29.8 fps** | 中偏浅 | **1800 帧 / 73 秒，零掉电** |
| 1 | 9.18 ms | **50 fps 封顶** | 很浅 | 4~9 秒必掉电（电流太大） |

### 为什么 60fps 做不到

单帧最快 = 刷新 9.18ms + 传输 4.9ms + 上传 1.4ms + 往返开销 ≈ **20ms → 50fps 封顶**。
要到 60fps 需要 16.6ms，只能靠差分帧 + 更高波特率再压传输；
而 50fps 下每 20ms 一次大电流脉冲，USB 口 4~9 秒就过流保护。

**30fps 是这颗芯片 + 这个供电的甜点。**

### 全刷的一个坑

清残影用 partial 波形刷白约 174ms。最初写成"连做两次"，即 348ms 不间断驱动 ——
结果每 600 帧触发全刷后，几十帧内必掉电。改成 **刷一次 → 关泵 → 歇 250ms → 重新开泵 → 再刷一次**
之后就稳了。这条经验可以推广成一句：**连续驱动超过约 300ms 就会被 USB 过流保护掐掉。**

---

## 实验记录

`scripts/` 里 26 个脚本对应 26 个问题，每个脚本的第一行注释写了它要验证什么：

| 脚本 | 问题 | 结论 |
|---|---|---|
| `t01` | 上电自检：时基/上传速度/流畅度 | 建立基线 |
| `t06` | 掉电是 SPI 上传引起还是面板驱动引起 | 都不是，与图案相关 |
| `t09` ~ `t10` | 是绘图代码越界，还是真实驱动？ | 绘图干净，是驱动电流 |
| `t15` ~ `t17` | 死亡与像素数的关系 | 与「同时放电的 source 线数」有关，阈值在 64~96 之间 |
| `t18` | 固定最坏图案，只改驱动电荷 | sumTP=1 能活，≥2 必挂 |
| `t21` ~ `t23` | 波形相位数与双向性 | `0x10` 单向 / `0x18` 双向 |
| `t24` ~ `t25` | 墨色与功耗的平衡点 | TP=6 最深，TP=2 能跑 30fps |

详细的实测表格见 [`docs/experiments.md`](docs/experiments.md)。

---

## 已知限制

- **1bpp 无灰阶**：只有黑白两态，抗锯齿的内容二值化后线条会抖。
- **残影无法根除**：连续局部刷新久了必须全刷一次（默认每 600 帧）。全刷会在画面里闪一下白。
- **帧内容变化量受限**：单帧大面积闪烁（例如整屏反转）在 sumTP≥2 时撑不住。
- **板子本身的问题**：这块 CH340 在刷新瞬间容易掉线重枚举，COM 号会变，`flash.py` 因此做了自动搜索。
- **温度未参与**：`0x1A` 可以写温度、`0x1D` 可以载入传感器值，但本项目没有做温度补偿，
  低温下墨色和速度都没有验证过。

---

## 关于版权

这个仓库**只包含我自己写的代码**：固件、PC 端工具、实验脚本、文档。

以下内容**刻意没有放进仓库**，请自行获取：

| 内容 | 原因 | 怎么拿到 |
|---|---|---|
| GxEPD2 库 | 第三方开源库（GPL-3.0），应由包管理器安装 | `arduino-cli lib install GxEPD2` |
| ESP8266 Arduino core | 同上 | 见「快速开始」 |
| IL3820 数据手册 | 厂商版权文档 | 从面板供应商处获取 |
| Bad Apple 视频与转换出的帧数据 | 动画版权归原作者，且体积大 | 自行准备视频后跑 `tools/badapple.py prep` |

`driver/exp_api.*` 里没有复制 GxEPD2 的任何代码，只调用它已有的受保护接口；
集成方式是"加两行 + 放两个文件"，不修改库的原文件。

固件依赖 GxEPD2（GPL-3.0），因此本项目的固件部分同样以 **GPL-3.0-or-later** 分发；
`tools/` 下的 PC 端脚本与本文档可以单独视为 **MIT**。详见 [LICENSE](LICENSE)。

思路来源：[TruElysia02/epd-fast](https://github.com/TruElysia02/epd-fast) 公开描述的
"单相波形 + 持续升压 + 双平面 HOLD" 三条原则。本项目是针对 IL3820 的独立实现，
两者的寄存器序列、波形数据和结论都不同（那边目标是 SSD1680）。
