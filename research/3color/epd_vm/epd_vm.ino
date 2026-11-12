/*
 * epd_vm —— 三色屏寄存器级脚本解释器（烧一次，之后改脚本不编译）
 * ==========================================================================
 * 为什么有它：
 *   每次改一个底层实验（写哪个寄存器、窗口多大、什么时候触发）都要重编译
 *   5 分钟，而实验本身只要 5 秒。把实验从 C++ 搬到"脚本数据"里就能解决。
 *
 * 分区（自管，不用核心内置文件系统）
 * --------------------------------------------------------------------------
 * ESP8266 没有 ESP32 那种 partitions.csv 分区表，整片 flash 就是一整块，
 * 谁用哪段完全由代码里的常量决定。下面是本固件的硬约定（LAYOUT 命令可打印）：
 *
 *   0x000000 - 0x0FFFFF    1MB   固件本体（链接脚本上限 1019KB，当前约 289KB）
 *   0x100000 - 0x5FFFFF    5MB   预留固件槽，现在空着（多槽引导用）
 *   0x600000 - 0xFF9FFF  9.98MB  数据区 ← 脚本存储，本文件只管这一段
 *   0xFFA000 - 0xFFAFFF    4KB   空隙（链接脚本 _FS_end 到 eeprom 之间）
 *   0xFFB000 - 0xFFBFFF    4KB   EEPROM 模拟扇区    ← 绝不能碰
 *   0xFFC000 - 0xFFCFFF    4KB   RF 校准           ← 绝不能碰
 *   0xFFD000 - 0xFFFFFF   12KB   WiFi 配置/MAC     ← 绝不能碰
 *
 * 最后 24KB 是出厂数据，写坏了板子还能启动，但 RF 参数和 MAC 会变成垃圾。
 * 所以数据区上界必须停在 0xFFA000，不能图整写成 0x1000000。
 *
 * 数据区布局：
 *   0x600000  头部   16B   'E','P','D','V', ver, count, payloadEnd(4B), 保留
 *   0x601000  目录   4096B 最多 60 条 × 64B: name[56] + off(4B) + len(4B)
 *   0x602000  负载           脚本正文，顺序追加
 *   写入时只擦「头部扇区 + 目录扇区 + 负载落在的那几个扇区」，
 *   不会整片擦写，避免无谓消耗寿命。
 *
 * ---------------------------------------------------------------------------
 * 串口命令（文本行 + '\n'，2 Mbps）
 *   兼容旧测试台，panel_test.py 不用改：
 *     P <id> <a> <b>   设图案（不刷新）
 *     F <c8|f7|d7> <n> 上传并用该模式刷 n 次，回报 avg/min/max
 *     M <n>            只测上传耗时
 *     I                状态
 *   脚本管理：
 *     PUT <name> <len> 随后发送 len 字节正文
 *     GET <name>       打印正文
 *     LS               列目录
 *     RM <name>        删除
 *     RUN <name>       执行
 *     FORMAT           清空数据区
 *   脚本动词见 HELP。
 * ---------------------------------------------------------------------------
 * 必须遵守的两条硬规则（都是踩出来的）：
 *   1) 触发前要先 0xF8 上电并装载 OTP 波形表，再用 0x32 覆盖 153 字节，
 *      否则时序照跑、BUSY 照释放、墨一点不动。
 *   2) 0x26 的 RAM 位 1 表示红，要黑白必须全写 0x00。
 * ==========================================================================
 */
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_3C.h>
#include <Adafruit_GFX.h>   // 只用它自带的 5x7 字模画启动画面

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define W 128
#define H 296
#define FBB ((W / 8) * H)

#define BOOT_HOLD_MS 2500        // 启动画面停留时间，随后整屏推黑+推白清场

#define DATA_BASE   0x600000UL
#define DATA_LIMIT  0xFFA000UL           // 系统区起点，绝对不能越过
#define DATA_SIZE   (DATA_LIMIT - DATA_BASE)   // 0x9FA000 = 9.98MB
#define HDR_ADDR    (DATA_BASE)
#define ENT_ADDR    (DATA_BASE + 0x1000UL)
#define PAY_ADDR    (DATA_BASE + 0x2000UL)
#define ENT_MAX     60
#define NAME_MAX    55

// 启动画面走横屏：逻辑画布 296x128，绘制时旋转成物理 128x296。
// 屏横过来拿时文字才是正的（FPC 在左侧，顺时针转 90° 摆放）。
#define LW 296
#define LH 128

#define VM_VERSION "1.1"
#define EPD_DRIVER "GxEPD2_290_C90c"
#define EPD_PANEL  "E029A10N 128x296"

static uint8_t fbBW[FBB] __attribute__((aligned(4)));
static uint8_t fbRD[FBB] __attribute__((aligned(4)));
static uint8_t upBuf[FBB] __attribute__((aligned(4)));   // 上传暂存（逐行拼窗口数据）
// 脚本缓冲提前定义：启动画面也要用它读 boot 文本。
// aligned(4) 必须加，spi_flash_read 目标地址不对齐会 Exception(9)。
static char scriptBuf[4096] __attribute__((aligned(4)));

// 153 字节黑白波形表（结构同 GxEPD2_213_BN::lut_partial，GPL-3.0-or-later）
static const uint8_t LUT_BW[153] = {
  0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

static char lineBuf[256];
static uint16_t lineLen = 0;
static uint32_t lastMs = 0;

// 当前窗口
static uint16_t winX = 0, winY = 0, winW = W, winH = H;

// ---------------------------------------------------------------------------
// 图案生成
// ---------------------------------------------------------------------------
static inline void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

static void fillAll(bool black)
{
  memset(fbBW, black ? 0x00 : 0xFF, FBB);
  memset(fbRD, 0x00, FBB);
}

static void makePattern(int id, int a, int b)
{
  fillAll(false);
  switch (id)
  {
    case 0: break;                                  // 全白
    case 1: fillAll(true); break;                   // 全黑
    case 2: {                                       // 竖条 w,p
      int w = a < 1 ? 1 : a, p = b < 1 ? 1 : b;
      if (w > p) w = p;
      for (int x = 0; x < W; x++) if (x % p < w) for (int y = 0; y < H; y++) px(x, y, true);
      break;
    }
    case 3: {                                       // 横条
      int hh = a < 1 ? 1 : a, p = b < 1 ? 1 : b;
      if (hh > p) hh = p;
      for (int y = 0; y < H; y++) if (y % p < hh) for (int x = 0; x < W; x++) px(x, y, true);
      break;
    }
    case 4: {                                       // 棋盘 s
      int s = a < 1 ? 1 : a;
      for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        if (((x / s) + (y / s)) & 1) px(x, y, true);
      break;
    }
    case 5: {                                       // 左侧实心块 pct%
      int w = W * (a < 0 ? 0 : (a > 100 ? 100 : a)) / 100;
      for (int y = 0; y < H; y++) for (int x = 0; x < w; x++) px(x, y, true);
      break;
    }
    case 6: {                                       // 灰阶 lvl/16
      int lvl = a < 0 ? 0 : (a > 16 ? 16 : a);
      int period = 16, on = lvl;
      for (int y = 0; y < H; y++) if (y % period < on) for (int x = 0; x < W; x++) px(x, y, true);
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------
// 启动画面
// ---------------------------------------------------------------------------
// 用 Adafruit_GFX 自带的 5x7 字模（随 GxEPD2 的依赖一起装，不用另带字体表），
// 把它的 drawPixel 重定向到我们自己的 fbBW —— 因为：
//   * GxEPD2_3C 的绘图 API 是分页式的，画不了全屏缓冲；
//   * writeNative 会丢掉红平面（踩过）。
// 位序注意：fbBW 里 0 才是黑，所以 color!=0 映射成黑。
class BufCanvas : public Adafruit_GFX
{
public:
  BufCanvas() : Adafruit_GFX(LW, LH) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override
  {
    if (x < 0 || y < 0 || x >= LW || y >= LH) return;
    // 横屏 -> 物理屏：顺时针转 90°。
    //   逻辑左上 (0,0)        -> 物理左下 (0, H-1)
    //   逻辑右上 (LW-1,0)     -> 物理左上 (0, 0)
    // 所以 px = 逻辑 y，py = H-1-逻辑 x。
    px(y, H - 1 - x, color != 0);
  }
};

static BufCanvas canvas;

// 启动画面的默认文本（flash 里没有 boot 脚本时用它）。
// 每行格式：<字号>|<文本>，字号 1~4；# 开头是注释。文本支持占位符：
//   %V 版本   %F CPU 频率(MHz)   %S flash 容量(MB)   %H 可用堆
//   %D 驱动类名   %P 屏型
#define BOOT_DEFAULT_TXT \
  "2|EPD VM  %V\n" \
  "1|%F MHz   %S MB flash   heap %H\n" \
  "1|driver  %D\n" \
  "1|panel   %P\n" \
  "2|BOOT OK\n"

// 把 %X 占位符换成实际值。不认识的占位符原样保留，方便自己加文字。
static void expandLine(char *dst, size_t dsz, const char *src)
{
  size_t o = 0;
  for (const char *p = src; *p && o + 32 < dsz; p++)
  {
    if (*p != '%') { dst[o++] = *p; continue; }
    char tmp[40] = { 0 };
    switch (p[1])
    {
      case 'V': snprintf(tmp, sizeof(tmp), "%s", VM_VERSION); break;
      case 'F': snprintf(tmp, sizeof(tmp), "%u", (unsigned)ESP.getCpuFreqMHz()); break;
      case 'S': snprintf(tmp, sizeof(tmp), "%u", (unsigned)(ESP.getFlashChipSize() >> 20)); break;
      case 'H': snprintf(tmp, sizeof(tmp), "%u", (unsigned)ESP.getFreeHeap()); break;
      case 'D': snprintf(tmp, sizeof(tmp), "%s", EPD_DRIVER); break;
      case 'P': snprintf(tmp, sizeof(tmp), "%s", EPD_PANEL); break;
      default:  dst[o++] = *p; continue;         // 不是占位符，原样输出
    }
    p++;                                          // 跳过占位符字母
    for (char *q = tmp; *q && o + 1 < dsz; q++) dst[o++] = *q;
  }
  dst[o] = 0;
}

// 逐行解析 "<字号>|<文本>"
static bool parseBootLine(char *p, size_t len, uint8_t *sz, char **txt, size_t *tl)
{
  if (len == 0 || p[0] == '#') return false;
  *sz = 2;
  *txt = p;
  if (len > 2 && p[0] >= '1' && p[0] <= '4' && p[1] == '|') { *sz = (uint8_t)(p[0] - '0'); *txt = p + 2; }
  *tl = (size_t)((p + len) - *txt);
  return *tl > 0;
}

static void drawBootScreen()
{
  fillAll(false);
  canvas.setTextColor(1, 0);
  canvas.setTextWrap(false);

  // 取 flash 里的 boot 文本；没有就用内置默认
  char *raw = scriptBuf;
  uint32_t n = storeRead("boot", (uint8_t *)raw, sizeof(scriptBuf) - 1);
  if (n == 0)
  {
    n = (uint32_t)strlen(BOOT_DEFAULT_TXT);
    memcpy(raw, BOOT_DEFAULT_TXT, n);
  }
  raw[n] = 0;

  // 第一遍：算总高，才能垂直居中
  int total = 0, cnt = 0;
  for (char *p = raw; *p && cnt < 16; )
  {
    char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    uint8_t sz; char *t; size_t tl;
    if (parseBootLine(p, len, &sz, &t, &tl)) { total += 8 * sz + 4; cnt++; }
    if (!e) break;
    p = e + 1;
  }
  int y = (LH - total) / 2;
  if (y < 2) y = 2;
  int y0 = y;

  // 第二遍：真正画
  cnt = 0;
  for (char *p = raw; *p && cnt < 16; )
  {
    char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    uint8_t sz; char *t; size_t tl;
    if (parseBootLine(p, len, &sz, &t, &tl))
    {
      char tmp[96], out[128];
      if (tl > sizeof(tmp) - 1) tl = sizeof(tmp) - 1;
      memcpy(tmp, t, tl); tmp[tl] = 0;
      expandLine(out, sizeof(out), tmp);
      int w = (int)strlen(out) * 6 * sz;
      canvas.setTextSize(sz);
      canvas.setCursor((LW - w) / 2, y);
      canvas.print(out);
      y += 8 * sz + 4;
      cnt++;
    }
    if (!e) break;
    p = e + 1;
  }

  Serial.printf("[boot] raw=%lu bytes  lines=%d  total=%d  y0=%d\r\n",
                (unsigned long)n, cnt, total, y0);
}

// ---------------------------------------------------------------------------
// 上传 + 触发
// ---------------------------------------------------------------------------
// 把窗口内的数据按行拼进 upBuf（窗口宽不是整屏时不能直接连续拷贝）
static uint32_t packWindow(const uint8_t *src, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t rowBytes = w / 8;
  uint16_t srcRowBytes = W / 8;
  uint16_t startByte = x / 8;
  for (uint16_t r = 0; r < h; r++)
    memcpy(upBuf + r * rowBytes, src + (y + r) * srcRowBytes + startByte, rowBytes);
  return (uint32_t)rowBytes * h;
}

static uint32_t uploadWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  display.epd2.expSetRamArea(x, y, w, h);
  uint32_t n = packWindow(fbBW, x, y, w, h);
  display.epd2.expWriteRam(0x24, upBuf, n);
  n = packWindow(fbRD, x, y, w, h);
  display.epd2.expWriteRam(0x26, upBuf, n);
  return n;
}

static uint32_t upload()
{
  return uploadWindow(winX, winY, winW, winH);
}

// mode: 0xC8 / 0xF7 / 0xD7 ...
static uint32_t fire(uint8_t mode, uint32_t timeout)
{
  display.epd2.expActivate(0xF8, 3000);            // 上电 + 装载 OTP 波形表
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));
  uint32_t t = millis();
  display.epd2.expActivate(mode, timeout);
  return millis() - t;
}

static uint8_t modeByte(const char *m)
{
  if (!m) return 0;
  if (!strcmp(m, "c8")) return 0xC8;
  if (!strcmp(m, "f7")) return 0xF7;
  if (!strcmp(m, "d7")) return 0xD7;
  if (!strcmp(m, "cc")) return 0xCC;
  if (!strcmp(m, "cf")) return 0xCF;
  return 0;
}

// ===========================================================================
// 脚本存储层
// ===========================================================================
// 目录扇区的暂存区。两个函数各写一份 static 会各占 4KB BSS，
// 而这块 DRAM 是从 heap 里扣的（heap 只剩 10KB），所以共用一份。
static uint32_t gEnts[1024];                     // 4096 字节 = 一个扇区

static uint32_t storePayloadEnd()
{
  uint32_t hdr[4];
  spi_flash_read(HDR_ADDR, hdr, 16);
  // 必须比完整的 magic。之前写成 (hdr[0] & 0xFFFF) != 0x4450 是错的：
  // 'E','P' 小端拼出来是 0x5045，不是 0x4450；判断永远失败 → 每次都返回 0
  // → 第二个脚本把第一个覆盖掉（LS 里两条记录都显示 @0x600000）。
  if (hdr[0] != 0x56445045) return 0;           // 'E','P','D','V'
  return hdr[2];
}

static void storeWriteHdr(uint32_t payloadEnd)
{
  uint32_t hdr[4];
  hdr[0] = 0x56445045;                          // 'E','P','D','V' 小端
  hdr[1] = 1;
  hdr[2] = payloadEnd;
  hdr[3] = 0;
  ESP.flashEraseSector(HDR_ADDR / 4096);
  ESP.flashWrite(HDR_ADDR, hdr, 16);
}

static bool storeFormat()
{
  storeWriteHdr(0);
  ESP.flashEraseSector(ENT_ADDR / 4096);
  return true;
}

static bool storeReady()
{
  uint32_t hdr[4];
  spi_flash_read(HDR_ADDR, hdr, 16);
  return (hdr[0] == 0x56445045);
}

// 目录项：64 字节 = name[56] + off(4) + len(4)
static void storeReadEntry(int i, char *name, uint32_t *off, uint32_t *len)
{
  uint32_t e[16];
  spi_flash_read(ENT_ADDR + i * 64, e, 64);
  memcpy(name, e, 56);
  name[55] = 0;
  *off = e[14];
  *len = e[15];
}

// 擦除后的 flash 全 0xFF，所以空槽有两种形态：
//   从未写过 → len = 0xFFFFFFFF
//   主动删除 → len = 0
// 只判 len==0 会让整片 0xFF 的目录被当成 60 条有效记录（这个坑踩过：
// LS 打印出一堆乱码 + "@0x5FFFFF"，因为 0x600000+0xFFFFFFFF 溢出）。
static inline bool entryEmpty(uint32_t len)
{
  return len == 0 || len == 0xFFFFFFFF;
}

static int storeFind(const char *name)
{
  char nm[56];
  uint32_t off, len;
  for (int i = 0; i < ENT_MAX; i++)
  {
    storeReadEntry(i, nm, &off, &len);
    if (entryEmpty(len)) break;                 // 空槽，后面不会再有有效项
    if (!strcmp(nm, name)) return i;
  }
  return -1;
}

static int storeCount()
{
  char nm[56];
  uint32_t off, len;
  for (int i = 0; i < ENT_MAX; i++)
  {
    storeReadEntry(i, nm, &off, &len);
    if (entryEmpty(len)) return i;
  }
  return ENT_MAX;
}

static bool storeSave(const char *name, const uint8_t *data, uint32_t len)
{
  if (len == 0 || len > 4096) return false;

  char nm[56];
  uint32_t off, olen;
  int idx = storeFind(name);
  uint32_t payloadEnd = storePayloadEnd();

  if (idx < 0)
  {
    idx = storeCount();
    if (idx >= ENT_MAX) return false;
  }
  else
  {
    // 覆盖已有脚本：沿用原来的偏移，允许原地改写
    storeReadEntry(idx, nm, &off, &olen);
    if (olen != len || (off % 4)) { idx = idx; }  // 长度变了就追加新副本
    else payloadEnd = off;
  }

  // 负载落点：必须从 PAY_ADDR 起算，不能从 DATA_BASE 起算。
  // 头部在 0x600000、负载若也落在 0x600000~0x600FFF 这同一个 4KB 扇区里，
  // storeWriteHdr() 最后擦扇区写头部时会把正文一起抹掉 —— 表现为
  // 写入成功、LS 长度正确、读回来全是 0xFF（启动画面解析出 0 行就是这么来的）。
  uint32_t at = PAY_ADDR + payloadEnd;
  if (at + len > DATA_BASE + DATA_SIZE) return false;

  // 写入：逐扇区「读出 -> 改掉那一段 -> 整扇区写回」。
  // 不能直接擦整个扇区再写，那样同一个 4KB 里的其他脚本会被抹成 0xFF。
  // 暂存借用 upBuf（4736 字节，够一个扇区），存脚本时不会同时在上传。
  {
    uint8_t *sec = upBuf;
    uint32_t first = at / 4096;
    uint32_t last = (at + len - 1) / 4096;
    for (uint32_t s = first; s <= last; s++)
    {
      uint32_t secStart = s * 4096;
      uint32_t copyStart = (at > secStart) ? at : secStart;
      uint32_t copyEnd = (at + len < secStart + 4096) ? (at + len) : (secStart + 4096);
      uint32_t n = copyEnd - copyStart;

      spi_flash_read(secStart, (uint32_t *)sec, 4096);
      memcpy(sec + (copyStart - secStart), data + (copyStart - at), n);
      ESP.flashEraseSector(s);
      for (uint32_t o = 0; o < 4096; o += 256)
        ESP.flashWrite(secStart + o, (uint32_t *)(sec + o), 256);
    }
  }

  // 写目录项：整扇区重写（只有 1 个扇区，成本可控）
  spi_flash_read(ENT_ADDR, gEnts, sizeof(gEnts));
  uint32_t *e = gEnts + idx * 16;
  memset(e, 0, 64);
  strncpy((char *)e, name, NAME_MAX);
  e[14] = payloadEnd;
  e[15] = len;
  ESP.flashEraseSector(ENT_ADDR / 4096);
  for (uint32_t o = 0; o < sizeof(gEnts); o += 256)
    ESP.flashWrite(ENT_ADDR + o, (uint32_t *)((uint8_t *)gEnts + o), 256);

  storeWriteHdr(payloadEnd + ((len + 3) & ~3u));
  return true;
}

static uint32_t storeRead(const char *name, uint8_t *dst, uint32_t maxLen)
{
  int i = storeFind(name);
  if (i < 0) return 0;
  char nm[56];
  uint32_t off, len;
  storeReadEntry(i, nm, &off, &len);
  if (len > maxLen) len = maxLen;
  spi_flash_read(PAY_ADDR + off, (uint32_t *)dst, (len + 3) & ~3u);
  return len;
}

static bool storeRemove(const char *name)
{
  int i = storeFind(name);
  if (i < 0) return false;
  spi_flash_read(ENT_ADDR, gEnts, sizeof(gEnts));
  // 后面的项整体前移一格，让目录保持连续。
  // 只 memset 留洞的话，storeCount 走到洞里就提前结束，后面的脚本会"消失"。
  int c = storeCount();
  for (int k = i; k < c - 1; k++) memcpy(gEnts + k * 16, gEnts + (k + 1) * 16, 64);
  memset(gEnts + (c - 1) * 16, 0, 64);
  ESP.flashEraseSector(ENT_ADDR / 4096);
  for (uint32_t o = 0; o < sizeof(gEnts); o += 256)
    ESP.flashWrite(ENT_ADDR + o, (uint32_t *)((uint8_t *)gEnts + o), 256);
  return true;
}

// ===========================================================================
// 脚本解释器
// ===========================================================================
static uint8_t runDepth = 0;

static int hexOrDec(const char *s)
{
  if (!s) return 0;
  return (int)strtol(s, NULL, 0);
}

// 执行一行脚本；返回 false 表示出错
static bool execLine(char *line)
{
  char *hash = strchr(line, '#');
  if (hash) *hash = 0;

  char *cmd = strtok(line, " \t\r");
  if (!cmd) return true;

  if (!strcmp(cmd, "WIN"))
  {
    winX = (uint16_t)hexOrDec(strtok(NULL, " \t"));
    winY = (uint16_t)hexOrDec(strtok(NULL, " \t"));
    winW = (uint16_t)hexOrDec(strtok(NULL, " \t"));
    winH = (uint16_t)hexOrDec(strtok(NULL, " \t"));
    if (winW == 0) winW = W;
    if (winH == 0) winH = H;
    return true;
  }
  if (!strcmp(cmd, "FULL")) { winX = 0; winY = 0; winW = W; winH = H; return true; }

  if (!strcmp(cmd, "GDR")) { display.epd2.expSetGdr((uint16_t)hexOrDec(strtok(NULL, " \t"))); return true; }

  if (!strcmp(cmd, "TEMP"))
  {
    int t = hexOrDec(strtok(NULL, " \t"));
    uint8_t d[2] = { (uint8_t)(t & 0xFF), (uint8_t)(t >> 8) };
    display.epd2.expWriteRam(0x1A, d, 2);      // 0x1A 是 2 字节，必须一次发完
    return true;
  }

  if (!strcmp(cmd, "BORDER")) { display.epd2.expRaw(0x3C, (uint8_t)hexOrDec(strtok(NULL, " \t"))); return true; }
  if (!strcmp(cmd, "VCOM"))   { display.epd2.expRaw(0x2C, (uint8_t)hexOrDec(strtok(NULL, " \t"))); return true; }

  if (!strcmp(cmd, "REG"))
  {
    uint8_t c = (uint8_t)hexOrDec(strtok(NULL, " \t"));
    char *a; uint8_t d[4]; uint8_t n = 0;
    while (n < 4 && (a = strtok(NULL, " \t"))) d[n++] = (uint8_t)hexOrDec(a);
    if (n == 1) display.epd2.expRaw(c, d[0]);
    else if (n > 1) display.epd2.expWriteRam(c, d, n);
    return true;
  }

  if (!strcmp(cmd, "PAT"))
  {
    int id = hexOrDec(strtok(NULL, " \t"));
    int a = hexOrDec(strtok(NULL, " \t"));
    int b = hexOrDec(strtok(NULL, " \t"));
    makePattern(id, a, b);
    return true;
  }

  if (!strcmp(cmd, "FILL"))
  {
    char *plane = strtok(NULL, " \t");
    int v = hexOrDec(strtok(NULL, " \t"));
    if (plane && (!strcmp(plane, "BW") || !strcmp(plane, "RD")))
    {
      // 只填当前窗口对应的字节
      uint16_t rowBytes = winW / 8, srcRow = W / 8, sb = winX / 8;
      for (uint16_t r = 0; r < winH; r++)
        memset((plane[0] == 'B' ? fbBW : fbRD) + (winY + r) * srcRow + sb, v, rowBytes);
    }
    return true;
  }

  if (!strcmp(cmd, "UP")) { upload(); return true; }

  if (!strcmp(cmd, "LUT")) { display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW)); return true; }

  if (!strcmp(cmd, "TRIG"))
  {
    uint8_t m = (uint8_t)hexOrDec(strtok(NULL, " \t"));
    if (!m) return false;
    uint32_t t = fire(m, 90000);
    Serial.printf("TRIG 0x%02X %lu ms\r\n", m, (unsigned long)t);
    return true;
  }

  if (!strcmp(cmd, "DELAY")) { delay((uint32_t)hexOrDec(strtok(NULL, " \t"))); return true; }

  if (!strcmp(cmd, "PRINT"))
  {
    char *rest = strtok(NULL, "");
    if (rest) Serial.printf("%s\r\n", rest);
    return true;
  }

  if (!strcmp(cmd, "RUN"))
  {
    char *n = strtok(NULL, " \t");
    if (!n || runDepth > 3) return false;
    static char nested[4096] __attribute__((aligned(4)));   // 同上，spi_flash_read 要求对齐
    uint32_t len = storeRead(n, (uint8_t *)nested, sizeof(nested) - 1);
    if (!len) { Serial.printf("ERR no script %s\r\n", n); return false; }
    nested[len] = 0;
    runDepth++;
    char *p = nested;
    while (*p)
    {
      char *e = strchr(p, '\n');
      if (e) *e = 0;
      execLine(p);
      if (!e) break;
      p = e + 1;
    }
    runDepth--;
    return true;
  }

  Serial.printf("ERR verb %s\r\n", cmd);
  return false;
}

static void runScript(const char *name)
{
  uint32_t len = storeRead(name, (uint8_t *)scriptBuf, sizeof(scriptBuf) - 1);
  if (!len) { Serial.printf("ERR no script %s\r\n", name); return; }
  scriptBuf[len] = 0;

  Serial.printf("RUN %s (%lu bytes)\r\n", name, (unsigned long)len);
  uint32_t t0 = millis();
  char *p = scriptBuf;
  while (*p)
  {
    char *e = strchr(p, '\n');
    if (e) *e = 0;
    if (*p) execLine(p);
    if (!e) break;
    p = e + 1;
  }
  Serial.printf("DONE %s %lu ms\r\n", name, (unsigned long)(millis() - t0));
}

// ===========================================================================
// 串口命令
// ===========================================================================
// 打印真实分区边界，方便随时核对"固件会不会写到数据区"
static void printLayout()
{
  Serial.println(F("--- flash layout (esp8266 无分区表，以下为代码硬约定) ---"));
  Serial.printf ("  0x000000-0x0FFFFF    1MB   firmware   built %lu KB / cap 1019KB\r\n",
                 (unsigned long)((ESP.getSketchSize() + 1023) / 1024));
  Serial.println(F("  0x100000-0x5FFFFF    5MB   reserved slots (empty)"));
  Serial.printf ("  0x%06lX-0x%06lX %5luKB   DATA       payload end 0x%06lX\r\n",
                 (unsigned long)DATA_BASE, (unsigned long)DATA_LIMIT,
                 (unsigned long)(DATA_SIZE / 1024),
                 (unsigned long)(PAY_ADDR + storePayloadEnd()));
  Serial.println(F("  0xFFA000-0xFFAFFF    4KB   gap"));
  Serial.println(F("  0xFFB000-0xFFBFFF    4KB   EEPROM  <- never touch"));
  Serial.println(F("  0xFFC000-0xFFCFFF    4KB   RF cal  <- never touch"));
  Serial.println(F("  0xFFD000-0xFFFFFF   12KB   WiFi/MAC<- never touch"));
  Serial.printf ("  free heap=%u\r\n", ESP.getFreeHeap());
}

static void printHelp()
{
  Serial.println(F("--- VM verbs ---"));
  Serial.println(F("WIN x y w h | FULL | GDR n | TEMP n | BORDER n | VCOM n"));
  Serial.println(F("REG cmd b0..b3 | PAT id a b | FILL BW|RD val | UP | LUT | TRIG mode"));
  Serial.println(F("DELAY ms | PRINT text | RUN name | # comment"));
  Serial.println(F("--- scripts ---"));
  Serial.println(F("PUT name len | GET name | LS | RM name | RUN name | FORMAT | LAYOUT"));
  Serial.println(F("SPLASH (重画启动画面) | BOOT (查看 boot 文本) | SELFTEST"));
}

static void handle(char *s)
{
  char *cmd = strtok(s, " \t");
  if (!cmd) return;

  if (!strcmp(cmd, "P"))
  {
    int id = hexOrDec(strtok(NULL, " \t"));
    int a = hexOrDec(strtok(NULL, " \t"));
    int b = hexOrDec(strtok(NULL, " \t"));
    makePattern(id, a, b);
    winX = 0; winY = 0; winW = W; winH = H;
    Serial.printf("OK P %d %d %d\n", id, a, b);
  }
  else if (!strcmp(cmd, "F"))
  {
    uint8_t m = modeByte(strtok(NULL, " \t"));
    int n = hexOrDec(strtok(NULL, " \t"));
    if (!m) { Serial.println("ERR mode"); return; }
    if (n < 1) n = 1;
    uint32_t sum = 0, mn = 0xFFFFFFFF, mx = 0;
    upload();
    for (int i = 0; i < n; i++)
    {
      uint32_t d = fire(m, 90000);
      sum += d; if (d < mn) mn = d; if (d > mx) mx = d;
    }
    lastMs = sum / n;
    Serial.printf("OK F %02X %d avg=%lu min=%lu max=%lu total=%lu\n",
                  m, n, (unsigned long)lastMs, (unsigned long)mn,
                  (unsigned long)mx, (unsigned long)sum);
  }
  else if (!strcmp(cmd, "M"))
  {
    int n = hexOrDec(strtok(NULL, " \t"));
    if (n < 1) n = 1;
    uint32_t t = millis();
    for (int i = 0; i < n; i++) upload();
    Serial.printf("OK M %d upload=%lu\n", n, (unsigned long)((millis() - t) / n));
  }
  else if (!strcmp(cmd, "LS"))
  {
    char nm[56]; uint32_t off, len;
    int c = storeCount();
    Serial.printf("OK LS %d\r\n", c);
    for (int i = 0; i < c; i++)
    {
      storeReadEntry(i, nm, &off, &len);
      if (len) Serial.printf("  %-20s %lu bytes @0x%06lX\r\n", nm, (unsigned long)len,
                             (unsigned long)(PAY_ADDR + off));
    }
  }
  else if (!strcmp(cmd, "GET"))
  {
    char *n = strtok(NULL, " \t");
    uint32_t len = n ? storeRead(n, (uint8_t *)scriptBuf, sizeof(scriptBuf) - 1) : 0;
    if (!len) { Serial.println("ERR none"); return; }
    scriptBuf[len] = 0;
    Serial.printf("OK GET %s %lu\r\n", n, (unsigned long)len);
    Serial.print(scriptBuf);
    Serial.print("\r\nEND\r\n");
  }
  else if (!strcmp(cmd, "RM"))
  {
    char *n = strtok(NULL, " \t");
    Serial.println(n && storeRemove(n) ? "OK RM" : "ERR none");
  }
  else if (!strcmp(cmd, "RUN"))
  {
    char *n = strtok(NULL, " \t");
    if (n) runScript(n);
  }
  else if (!strcmp(cmd, "FORMAT"))
  {
    storeFormat();
    Serial.println("OK FORMAT");
  }
  else if (!strcmp(cmd, "HELP"))
  {
    printHelp();
  }
  else if (!strcmp(cmd, "SELFTEST"))
  {
    // 换屏后先跑这条：画 8 黑 / 8 白横条纹并整屏刷一次。
    // 屏上出现条纹 = 排线到位、固件能驱动；全白不动 = 排线没插好（③号踩过的坑）。
    makePattern(3, 8, 16);
    winX = 0; winY = 0; winW = W; winH = H;
    upload();
    uint32_t t = fire(0xF7, 90000);
    Serial.printf("OK SELFTEST stripes(8/16) %lu ms\r\n", (unsigned long)t);
  }
  else if (!strcmp(cmd, "SPLASH"))
  {
    // 改完 boot 文本不用重启，重画一遍立刻看效果
    drawBootScreen();
    winX = 0; winY = 0; winW = W; winH = H;
    // 自查：fbBW 里到底画进去多少黑点。0 说明绘制没生效，
    // 有黑点但屏白则说明问题在 upload/fire，一步就能分清。
    uint32_t dark = 0;
    for (uint32_t i = 0; i < FBB; i++) dark += (uint32_t)__builtin_popcount((uint8_t)~fbBW[i]);
    uint32_t up = upload();
    uint32_t t = fire(0xF7, 90000);
    Serial.printf("OK SPLASH %lu ms dark=%lu up=%lu\r\n",
                  (unsigned long)t, (unsigned long)dark, (unsigned long)up);
  }
  else if (!strcmp(cmd, "BOOT"))
  {
    uint32_t n = storeRead("boot", (uint8_t *)scriptBuf, sizeof(scriptBuf) - 1);
    if (!n) { Serial.println("OK BOOT (default, flash 里没有 boot)"); return; }
    scriptBuf[n] = 0;
    Serial.printf("OK BOOT %lu bytes\r\n%s\r\n", (unsigned long)n, scriptBuf);
  }
  else if (!strcmp(cmd, "LAYOUT"))
  {
    printLayout();
  }
  else if (!strcmp(cmd, "I"))
  {
    Serial.printf("OK I heap=%u data=0x%06lX used=%lu win=%u,%u,%u,%u\r\n",
                  ESP.getFreeHeap(), (unsigned long)DATA_BASE,
                  (unsigned long)storePayloadEnd(),
                  winX, winY, winW, winH);
  }
  else if (!strcmp(cmd, "PUT"))
  {
    // PUT <name> <len>  随后接收 len 字节
    char *n = strtok(NULL, " \t");
    int len = hexOrDec(strtok(NULL, " \t"));
    if (!n || len <= 0 || len > 4000) { Serial.println("ERR args"); return; }
    Serial.printf("READY %d\r\n", len);
    uint32_t got = 0;
    uint32_t t0 = millis();
    while (got < (uint32_t)len && millis() - t0 < 5000)
    {
      int avail = Serial.available();
      if (avail > 0)
      {
        int n2 = Serial.readBytes(scriptBuf + got, len - got < avail ? len - got : avail);
        got += n2;
      }
    }
    if (got != (uint32_t)len) { Serial.printf("ERR short %lu\r\n", (unsigned long)got); return; }
    Serial.println(storeSave(n, (uint8_t *)scriptBuf, len) ? "OK PUT" : "ERR save");
  }
  else
  {
    Serial.println("ERR cmd");
  }
}

// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("EPD VM v1"));

  display.init(115200, true, 10, false);
  Serial.begin(2000000);
  delay(100);

  if (!storeReady()) storeFormat();

  winX = 0; winY = 0; winW = W; winH = H;

  // ---- 1) 启动画面 ----
  drawBootScreen();
  upload();
  uint32_t t0 = millis();
  fire(0xF7, 90000);                       // 0xF7 = OTP 完整波形，质量优先
  Serial.printf("boot screen %lu ms\r\n", (unsigned long)(millis() - t0));
  delay(BOOT_HOLD_MS);

  // ---- 2) 彻底清屏 ----
  // 单刷白会把启动画面的黑字留成淡影。先整屏推黑再推白，
  // 颗粒被两个方向各推一遍，是最干净的清场方式。
  fillAll(true);  upload(); fire(0xF7, 90000);
  fillAll(false); upload(); fire(0xF7, 90000);

  Serial.printf("READY heap=%u scripts=%d\r\n", ESP.getFreeHeap(), storeCount());
}

void loop()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (lineLen)
      {
        lineBuf[lineLen] = 0;
        handle(lineBuf);
        lineLen = 0;
      }
    }
    else if (lineLen < sizeof(lineBuf) - 1)
    {
      lineBuf[lineLen++] = c;
    }
  }
}
