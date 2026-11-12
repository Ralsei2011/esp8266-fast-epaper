/*
 * 三色屏纯黑白快刷 —— 第二轮：修掉第一轮的两个致命缺陷
 * ==========================================================================
 * 第一轮"112ms 但画面没动静"的根因（已定位，两条叠加）：
 *
 *   A. GxEPD2_290_C90c::writeNative(data1, data2, ...) 只转发 data1，
 *      data2（红色平面）被整个丢弃 → 0x26 从来没被正确更新。
 *   B. writeImage 内部调用 _Init_Part() == _InitDisplay()，里面有 0x12 SWRESET
 *      → 每次上传数据都软复位，把 0x32 写进去的自定义 LUT 清空。
 *      于是 "写 LUT → 上传数据 → 0x22=0xCC" 这个顺序必然失败：LUT 已被清掉，
 *      0xCC 拿到的是一张全零波形表，12 相空转 112ms，墨水不动。
 *
 * 本轮做法：全部走裸通道，不调用任何 GxEPD2 高层函数。
 *   顺序固定为：设窗口 → 0x24 → 0x26 → 上电(0xC0，不碰 LUT) → 0x32 → 0x22
 *
 * 分阶段（每阶段停 8 秒，屏上变化都极大，肉眼不会看错）：
 *   S1  OTP 全黑      —— 判 0x26 极性（若变红说明极性反了）
 *   S2  OTP 测试图    —— 判裸通道数据链路是否正确（必须有图案）
 *   S3  LUT + 0xCC 反相图 —— 有变化 = LUT 生效
 *   S4  LUT + 0xCC 正相图 —— 再变回来
 *   S5  扫 mode：0xC4 / 0xC8，看哪个能出图
 *   S6  OTP 恢复白
 *
 * 串口 115200。全程约 2~3 分钟。
 * ==========================================================================
 */
#define ENABLE_GxEPD2_GFX 1

#include <SPI.h>
#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define W 128
#define H 296
#define FBB ((W / 8) * H)          // 4736

static uint8_t fbBW[FBB] __attribute__((aligned(4)));   // 1=白, 0=黑
static uint8_t fbRD[FBB] __attribute__((aligned(4)));   // 0x26 RAM: 1=非红, 0=红

// ---------------------------------------------------------------------------
// 153 字节 LUT：SSD1680 黑白局刷波形（GxEPD2_213_BN::lut_partial 同构）
//   前 60 字节 = LUT0..LUT4 的 VS 表（12 相 × 5 组）
//   后 93 字节 = TP 表 + 帧率段
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
static inline void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

// 全屏白底 + 红平面全"非红"
static void clearAll() { memset(fbBW, 0xFF, FBB); memset(fbRD, 0xFF, FBB); }

static void drawTest(bool invert)
{
  clearAll();
  // 左上 48x44 实心块
  for (int y = 20; y < 64; y++) for (int x = 10; x < 58; x++) px(x, y, !invert);
  // 右上 1px 竖线
  for (int x = 70; x < 118; x += 4) for (int y = 20; y < 64; y++) px(x, y, !invert);
  // 中间 4x4 棋盘
  for (int y = 80; y < 160; y++)
    for (int x = 10; x < 118; x++)
      if (((x / 4) + (y / 4)) % 2 == 0) px(x, y, !invert);
  // 下方 1px 横线
  for (int y = 180; y < 260; y += 2) for (int x = 10; x < 118; x++) px(x, y, !invert);
  // 最底 30x16 小块
  for (int y = 270; y < 286; y++) for (int x = 10; x < 40; x++) px(x, y, !invert);
}

// 裸写两帧平面：不调用 writeNative / writeImage（它们会 SWRESET 并丢红平面）
static void uploadRaw()
{
  display.epd2.expSetRamArea(0, 0, W, H);
  display.epd2.expWriteRam(0x24, fbBW, FBB);
  display.epd2.expWriteRam(0x26, fbRD, FBB);
}

static uint32_t activate(uint8_t mode, uint32_t to)
{
  uint32_t t = millis();
  display.epd2.expActivate(mode, to);
  return millis() - t;
}

// 完整快刷路径：上电(不碰 LUT) → 0x32 写 LUT → 0x22 触发
static uint32_t lutRefresh(uint8_t mode)
{
  display.epd2.expActivate(0xC0, 800);      // EN_CLK + EN_ANALOG，不 LOAD_LUT
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));
  return activate(mode, 40000);
}

static void hold(const char *tag, uint32_t ms)
{
  Serial.printf("    >>> 观察 %lu ms  [%s]\n", ms, tag);
  delay(ms);
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("############ 3C BW-FAST v2 ############");
  Serial.printf("BUSY idle=%d  heap=%u\n", digitalRead(PIN_BUSY), ESP.getFreeHeap());

  uint32_t t0 = millis();
  display.init(115200, true, 10, false);   // 内部含一次 OTP 全刷
  Serial.printf("[init] %lu ms   heap=%u\n", millis() - t0, ESP.getFreeHeap());

  // ---------- S1：OTP 全黑，判 0x26 极性 ----------
  Serial.println("\n[S1] OTP 全黑（BW=0x00  RED=0xFF）");
  memset(fbBW, 0x00, FBB);
  memset(fbRD, 0xFF, FBB);
  uploadRaw();
  uint32_t t1 = activate(0xF7, 40000);
  Serial.printf("[S1] %lu ms   若屏变红=极性反，若变黑=极性对\n", t1);
  hold("S1 全黑", 8000);

  // ---------- S2：OTP 测试图，判裸通道数据链路 ----------
  Serial.println("\n[S2] OTP 测试图（必须出现图案，否则裸通道有问题）");
  drawTest(false);
  uploadRaw();
  uint32_t t2 = activate(0xF7, 40000);
  Serial.printf("[S2] %lu ms\n", t2);
  hold("S2 测试图", 10000);

  // ---------- S3：自定义 LUT + 0xCC，刷反相图 ----------
  Serial.println("\n[S3] LUT + 0x22=0xCC 刷反相图");
  drawTest(true);
  uploadRaw();
  uint32_t t3 = lutRefresh(0xCC);
  Serial.printf("[S3] %lu ms   有变化 = 自定义 LUT 真的生效\n", t3);
  hold("S3 反相图", 10000);

  // ---------- S4：同 LUT 刷回正相，验证可重复 ----------
  Serial.println("\n[S4] LUT + 0xCC 刷回正相图");
  drawTest(false);
  uploadRaw();
  uint32_t t4 = lutRefresh(0xCC);
  Serial.printf("[S4] %lu ms\n", t4);
  hold("S4 正相图", 10000);

  // ---------- S5：换 mode 字节扫描 ----------
  Serial.println("\n[S5] mode 字节扫描（同一张反相图，只改 0x22）");
  const uint8_t modes[] = {0xC4, 0xC8, 0xCF};
  const char *names[] = {"0xC4 EN_CLK|EN_AN|MODE1", "0xC8 EN_CLK|EN_AN|MODE2", "0xCF 含DIS_AN(可能无效)"};
  uint32_t s5[3];
  for (int i = 0; i < 3; i++)
  {
    drawTest(i % 2 == 0);
    uploadRaw();
    s5[i] = lutRefresh(modes[i]);
    Serial.printf("[S5.%d] %s -> %lu ms\n", i + 1, names[i], s5[i]);
    hold(names[i], 7000);
  }

  // ---------- S6：OTP 恢复白 ----------
  Serial.println("\n[S6] OTP 全刷回白");
  clearAll();
  uploadRaw();
  uint32_t t6 = activate(0xF7, 40000);
  Serial.printf("[S6] %lu ms\n", t6);

  Serial.println("\n########## DONE ##########");
  Serial.printf("OTP全黑=%lu  OTP图=%lu  LUT反相=%lu  LUT正相=%lu\n", t1, t2, t3, t4);
  Serial.printf("mode扫描: C4=%lu  C8=%lu  CF=%lu\n", s5[0], s5[1], s5[2]);
  Serial.println("判读：S3/S4 若 <2000ms 且屏上有图 = 三色屏黑白快刷打通");
}

void loop()
{
  delay(60000);
  Serial.printf("[idle] heap=%u\n", ESP.getFreeHeap());
}
