# epd_vm —— 三色屏寄存器级脚本解释器

烧一次，之后改实验只改脚本，不再重编译。

## 为什么要有它

调底层实验时，改一行寄存器操作要重编译 5 分钟，而实验本身只跑 5 秒。
这里把"实验"从 C++ 挪进 **flash 里的文本脚本**：PC 端 `PUT` 一份脚本文本，`RUN` 执行，
板子按行解释成 SPI 操作。换屏、换参数、换窗口，都不用碰编译器。

本目录的 8 步窗口矩阵（`win_matrix.epd`，1266 字节）就是这样在五块屏上各跑了一遍，
见 `../partial-refresh.md` 的「五屏复现」。

## 存储布局（自己管，不用核心的文件系统）

ESP8266 没有 ESP32 那种分区表，整片 16MB 是一整块，谁用哪段全由代码常量决定。
`LAYOUT` 命令可以随时打印这张表：

| 地址范围 | 大小 | 用途 |
|---|---|---|
| `0x000000`–`0x0FFFFF` | 1 MB | 固件本体（当前约 289 KB） |
| `0x100000`–`0x5FFFFF` | 5 MB | 预留固件槽，空着 |
| `0x600000`–`0xFF9FFF` | ~10 MB | **数据区**：脚本存储 |
| `0xFFA000`–`0xFFAFFF` | 4 KB | 空隙（`_FS_end` 到 eeprom 之间） |
| `0xFFB000`–`0xFFBFFF` | 4 KB | EEPROM 模拟 ← 绝不能碰 |
| `0xFFC000`–`0xFFCFFF` | 4 KB | RF 校准 ← 绝不能碰 |
| `0xFFD000`–`0xFFFFFF` | 12 KB | WiFi 配置 / MAC ← 绝不能碰 |

最后 24 KB 是出厂数据，写坏了板子还能启动，但 RF 参数和 MAC 会变成垃圾。
所以数据区上界必须停在 `0xFFA000`，不能图整写成 `0x1000000`。

数据区内部：

```
0x600000  头部    16B    'E','P','D','V', ver, count, payloadEnd(4B), 保留
0x601000  目录  4096B    最多 60 条 × 64B：name[56] + off(4B) + len(4B)
0x602000  负载           脚本正文，顺序追加
```

三者必须各占独立的 4 KB 扇区。**早期版本把负载紧跟头部放，结果写完脚本屏全白** ——
`storeWriteHdr()` 擦除整个头部扇区再写 16 字节，把同一扇区里的正文擦成 0xFF 了。
写入现在只擦「头部扇区 + 目录扇区 + 负载用到的扇区」，且头部走"读-改-写"，不整片擦。

## 串口命令（文本行，2 Mbps）

脚本管理：

```
PUT <name> <len>   随后发送 len 字节正文，回 OK PUT
GET <name>         打印正文 + END
LS                 列目录
RM <name>          删除
RUN <name>         执行，结束打印 DONE <name> <ms>
FORMAT             清空数据区
LAYOUT             打印上面那张地址表
```

脚本动词：

```
WIN x y w h        设 RAM 窗口（0x44/0x45/0x4e/0x4f 一起）
FULL               窗口恢复整屏
GDR n              门线数（0x01），n=296 全屏
TEMP n             温度覆写（0x1A + 0x22=0xD7 配合）
BORDER n           border 波形（0x3C）
VCOM n             VCOM 电压
REG cmd b0..b3     裸写寄存器，最多 4 个参数字节
PAT id a b         设图案（不刷新）
FILL BW|RD val     用常量填某个平面
UP                 把帧缓冲推到屏上（两个平面都写）
LUT                0x32 覆盖 153 字节波形表
TRIG mode          0xF8 上电 + 装载 OTP 表，再 0x22=<mode> 触发；回 TRIG 0x?? <ms> ms
DELAY ms           等待
PRINT text         串口打印
RUN name           脚本内嵌套调用
# comment          注释
```

`TRIG` 的 mode 要写数字（`TRIG 0xF7`），不能写 `f7`，参数走 `hexOrDec`。

启动画面相关：

```
SPLASH    按 flash 里的 boot 文本重画，并自报 dark=<黑点数> up=<上传字节>
BOOT      查看当前 boot 文本
SELFTEST  自检
```

`boot` 记录是 `<字号>|<文本>` 每行一条，支持占位符 `%V` 版本、`%F` 频率、`%S` 驱类、
`%H` 分辨率、`%D` 驱动名、`%P` 屏型号。改开机画面不用重编译：
`python tools/setboot.py boot_example.txt` 写完自动发 `SPLASH`。

## 两条硬规则（都是踩出来的）

1. 触发前必须先 `0xF8` 上电并装载 OTP 波形表，再用 `0x32` 覆盖 153 字节，
   否则时序照跑、BUSY 照释放、墨一点不动。
2. `0x26` 平面的位 1 表示红，要黑白必须全写 `0x00`。

## 配套工具

```bash
python tools/run_script.py scripts/win_matrix.epd w8 --panel 5   # PUT + RUN，落 CSV
python tools/run_only.py  w8 --panel 3                            # 只 RUN（脚本已在 flash，省寿命）
python tools/summarize_panels.py --model                          # 跨屏汇总 + 拟合 a/b
python tools/setboot.py   boot_example.txt                        # 改开机文本
python tools/preview_boot.py boot_example.txt                     # 不开屏先看排版
```

`run_only.py` 内置数据体检：如果某步耗时明显短于该型号正常值，判定为 BUSY 线接触不良
（`waitBusy` 提前返回，会伪装成"屏变快了"），结果写进 `results/rejected/` 并提示重插排线。
