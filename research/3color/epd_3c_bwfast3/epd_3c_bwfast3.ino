/*
 * 三色屏纯黑白快刷 —— 第四轮：修正 0x26 极性后锁定 0xC8 通道
 * ==========================================================================
 * 三轮回溯：
 *   v1  112ms 无画面：writeNative 丢弃红平面 + writeImage 内 SWRESET 清 LUT
 *   v2  修掉上述两坑后发现模式位决定走哪套波形：
 *         0xCC/0xC4/0xCF -> 2955ms    0xC8(MODE2) -> 92ms
 *   v3  实测 0x26 极性：RED=0xFF 刷出的是红色，RED=0x00 才是"非红"
 *       （与 GxEPD2 内部 writeImage 对 0x26 取反是自洽的：
 *        color buffer 里 1=红，写 0x26 时取反 -> RAM 0，所以 RAM 0=红、
 *        RAM 1=非红。之前按"1=非红"理解，导致每次都在推红底。）
 *
 * 本轮：RED 全 0x00，验证 0xC8 能否刷出正确黑白画面。
 *
 *   P1  OTP 全白   RED=0x00   -> 必须纯白
 *   P2  OTP 测试图 RED=0x00   -> 黑图案 + 白底，不该有红
 *   P3  0xC8 全白
 *   P4  0xC8 全黑
 *   P5  0xC8 棋盘
 *   P6  0xC8 反相棋盘
 *   P7  0xC8 全白
 *   P8  OTP 恢复白
 *
 * 串口 115200，全程约 3 分钟。
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
#define FBB ((W / 8) * H)

static uint8_t fbBW[FBB] __attribute__((aligned(4)));   // 1=白 0=黑
static uint8_t fbRD[FBB] __attribute__((aligned(4)));   // 0x26 RAM: 1=红 0=非红

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

static inline void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

static void setBW(bool black) { memset(fbBW, black ? 0x00 : 0xFF, FBB); }
// RED 位 1 = 红。要显示纯黑白，这里必须是 0x00。
static void noRed() { memset(fbRD, 0x00, FBB); }
static void allRed() { memset(fbRD, 0xFF, FBB); }

static void drawTest(bool invert)
{
  setBW(false); noRed();
  for (int y = 20; y < 64; y++) for (int x = 10; x < 58; x++) px(x, y, !invert);
  for (int x = 70; x < 118; x += 4) for (int y = 20; y < 64; y++) px(x, y, !invert);
  for (int y = 80; y < 160; y++)
    for (int x = 10; x < 118; x++)
      if (((x / 4) + (y / 4)) % 2 == 0) px(x, y, !invert);
  for (int y = 180; y < 260; y += 2) for (int x = 10; x < 118; x++) px(x, y, !invert);
  for (int y = 270; y < 286; y++) for (int x = 10; x < 40; x++) px(x, y, !invert);
}

static void checker(bool invert)
{
  setBW(false); noRed();
  for (int y = 40; y < 240; y++)
    for (int x = 8; x < 120; x++)
      if ((((x / 4) + (y / 4)) % 2 == 0) != invert) px(x, y, true);
}

static void uploadRaw()
{
  display.epd2.expSetRamArea(0, 0, W, H);
  display.epd2.expWriteRam(0x24, fbBW, FBB);
  display.epd2.expWriteRam(0x26, fbRD, FBB);
}

static uint32_t act(uint8_t mode, uint32_t to)
{
  uint32_t t = millis();
  display.epd2.expActivate(mode, to);
  return millis() - t;
}

static uint32_t fast08()
{
  display.epd2.expActivate(0xC0, 800);
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));
  return act(0xC8, 20000);
}

static void hold(const char *tag)
{
  Serial.printf("    >>> 看屏 15s  [%s]\n", tag);
  delay(15000);
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("############ 3C 0xC8 PROBE v2 (RED fixed) ############");

  display.init(115200, true, 10, false);

  Serial.println("\n[P1] OTP 全白  RED=0x00 -> 应纯白");
  setBW(false); noRed(); uploadRaw();
  Serial.printf("[P1] %lu ms\n", act(0xF7, 40000));
  hold("P1 纯白");

  Serial.println("\n[P2] OTP 测试图 RED=0x00 -> 黑图案+白底，无红");
  drawTest(false); uploadRaw();
  Serial.printf("[P2] %lu ms\n", act(0xF7, 40000));
  hold("P2 测试图");

  Serial.println("\n[P3] 0xC8 全白");
  setBW(false); noRed(); uploadRaw();
  Serial.printf("[P3] %lu ms\n", fast08());
  hold("P3 全白");

  Serial.println("\n[P4] 0xC8 全黑");
  setBW(true); noRed(); uploadRaw();
  Serial.printf("[P4] %lu ms\n", fast08());
  hold("P4 全黑");

  Serial.println("\n[P5] 0xC8 棋盘");
  checker(false); uploadRaw();
  Serial.printf("[P5] %lu ms\n", fast08());
  hold("P5 棋盘");

  Serial.println("\n[P6] 0xC8 反相棋盘");
  checker(true); uploadRaw();
  Serial.printf("[P6] %lu ms\n", fast08());
  hold("P6 反相棋盘");

  Serial.println("\n[P7] 0xC8 全白");
  setBW(false); noRed(); uploadRaw();
  Serial.printf("[P7] %lu ms\n", fast08());
  hold("P7 全白");

  Serial.println("\n[P8] OTP 恢复白");
  setBW(false); noRed(); uploadRaw();
  Serial.printf("[P8] %lu ms\n", act(0xF7, 40000));

  Serial.println("\n########## DONE ##########");
  Serial.println("判读：P3/P4/P5/P6 都有正确画面且 ~92ms = 打通");
}

void loop()
{
  delay(60000);
  Serial.printf("[idle] heap=%u\n", ESP.getFreeHeap());
}
