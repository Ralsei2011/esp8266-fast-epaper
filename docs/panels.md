# 面板档案

本项目用过的所有面板。型号来自 FPC 上的丝印，IC 型号来自公开规格书与实测。

---

## 三色屏（红黑白）

型号统一为 **HINK-E029A10**，佳创力（Chaitronix）的玻璃基板电子纸，后接批次/流水号。

| 编号 | 丝印 | 电压 | 使用状态 |
|---|---|---|---|
| ① | `E029A10N184274` | 4.68V | 首批实验用这块 |
| ② | `E029A10N184274` | 4.68V | 待测 |
| ③ | `E029A10N184267` | 4.68V | 待测 |
| ④ | `E029A10N1843A3` | 4.68V | 待测 |
| ⑤ | `E029A10N184299` | 4.68V | 待测 |

①和②流水号完全相同，③④⑤各差几位，是不同批次。

### 面板规格

| 项目 | 值 |
|---|---|
| 尺寸 | 2.9 英寸 |
| 分辨率 | 296 × 128 |
| 显示颜色 | 黑 / 白 / 红 |
| 有效区 | 66.90 × 29.05 mm |
| 外形 | 79.0 × 36.7 × 1.05 mm |
| 像素间距 | 0.227 × 0.226 mm |
| DPI | 112 |
| 驱动 IC | **SSD1680ZA** |
| 接口 | 4 线 SPI |
| 工作温度 | 0 ~ +40 °C |

### 引脚里最关键的一条

第 5 脚是 `VSH2` —— **Positive Source driving voltage (Red)**。

也就是说红色不是靠 VSH1/VSL 差分推出来的，而是有一条**独立的正电压轨**专门伺候红色颜料。这从硬件层面解释了为什么：

- 红色只能在完整 OTP 波形里迁移（需要 VSH2 参与多相序列）
- 局刷波形不碰 VSH2，所以推不动红
- 一旦 `0x26` 置位要求显示红，整帧必须走慢路径

### 数据约定（实测确认）

| 命令 | 含义 |
|---|---|
| `0x24` | 黑白平面，位 1 = 白，0 = 黑 |
| `0x26` | 彩色平面，位 1 = 红，0 = 非红 |

注意 `0x26` 的极性和 GxEPD2 内部写法是自洽的：`writeImage` 写 `0x26` 时对数据取反，而 `color` buffer 里 1 表示红，取反后存进 RAM 的是 0。所以**要显示纯黑白，`0x26` 必须全写 `0x00`**。

---

## 双色屏（黑白）

| 丝印 | 说明 |
|---|---|
| `E029A01N16AF28` | HINK-E029A01 系列，黑白，2.9" 296×128 |

驱动 IC 为 **IL3820**，与三色屏完全不同的控制器。这也是为什么两者的低层经验不能直接照搬：

| | IL3820（双色） | SSD1680ZA（三色） |
|---|---|---|
| LUT | 寄存器可写，`0x32` 立即生效 | `0x32` 写了也不响应 |
| `0x26` | 旧帧缓冲（previous frame） | 彩色平面 |
| 局刷 | 真差分，9.18ms | 无差分波形，只能缩窗口 + 全波形 |
| 最快单帧 | 9.18 ms | 92 ms |

---

## 关于"为什么双色能局刷、三色不行"

这不是驱动写错了，是**墨水本身的物理限制**。

三色屏里的红色颗粒比黑白颗粒更重，需要更长时间的电场才能迁移到表面。双色屏的局刷靠的是"差分波形"——只把需要变的像素推开一小段，所以几毫秒就够。三色屏没有这套差分波形（OTP 里根本没烧），每次刷新都必须跑完整的多相序列，把颗粒推到确定位置。

第三方实机测试文档（epdsi）的结论一致：

> Colour panels have no fast/differential waveform. The red pigment is a heavier particle that needs the full OTP waveform to migrate.

以及关于 fast LUT 的警告：

> `0xFC` selects the controller's built-in fast LUT. That LUT only exists for monochrome panels. On a colour panel it is not a speed-up and actively breaks the image: the update runs at full-refresh speed anyway, and because the fast path only rewrites the Black/White RAM, all red content is dropped.

也就是说：**三色屏上不存在"只刷一小块就快"这回事**。缩小 RAM 窗口只会缩小重绘区域，耗时仍是完整一波形。GxEPD2 里 `GxEPD2_154_Z90c` 的 `partial_refresh_time == full_refresh_time`、`hasFastPartialUpdate == false`，同一个事实。

---

## 实测对比（`f7` 慢波形，27 种图案各刷一次）

统一固件 `epd_3c_stream`，电脑端 `tools/panel_test.py`，测试前后各刷一次全白。

| 编号 | 丝印 | 均值 | 与①差 | 目视结论 |
|---|---|---|---|---|
| ① | `184274` | 5067.5 ms | — | 细条可分辨、棋盘分明、全黑够黑、边角均匀，无泛红无白雾 |
| ② | `184274` | 5082.7 ms | +15.2 ms（+0.3%） | 同上，未见异常 |
| ③ | `184267` | — | — | 待测 |
| ④ | `1843A3` | — | — | 待测 |
| ⑤ | `184299` | — | — | 待测 |

①② 的流水号完全相同，耗时却有稳定 15ms 偏移，27 种图案一致。说明**同一型号、同一 IC 的不同玻璃，OTP 里的波形相数或帧时间确实会有差异**，与第三方实机文档"same IC, different OTP waveform"的结论一致。

原始数据：`results/panel1_f7.csv`、`results/panel2_f7.csv`。

### 测试方法

```bash
python tools/panel_test.py --panel 2 --mode f7 --frames 1 --hold 2
```

- 图案由固件现算（27 种：1px 竖条/横条、4 档棋盘、4 档实心、3 档灰阶、全白全黑）
- `f7` = OTP 慢波形，约 5.07 秒一张，质量优先
- `--hold 2` 每张停 2 秒供目视
- 测试前后自动刷全白

---

## 待验证

- 五块屏在同一组条纹图案下的墨色一致性（本项目 `epd_3c_stream` + `panel_test.py`）
- 批次号 184274 / 184267 / 1843A3 / 184299 是否对应不同 OTP 波形（同 IC 不同玻璃，波形可能不同）
- 92ms 那条路径是否压得动（已证伪：改 GDR、改 LUT 都无效）
