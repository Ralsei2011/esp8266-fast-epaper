/*
 * 三色屏纯黑白快刷 —— 第五轮：压 TP 帧数，找墨色与速度的拐点
 * ==========================================================================
 * 前四轮结论：
 *   v1  writeNative 丢红平面 + writeImage 内 SWRESET 清 LUT -> 什么都不显示
 *   v2  裸通道 + 0xC8(MODE2) -> 92ms（0xCC/0xC4 走另一套长波形 2955ms）
 *   v3  0x26 极性修正（RAM 1=红，要黑白必须全 0x00）-> 画面正确
 *   v4  实测：全白/全黑/棋盘/反相棋盘全部正确，稳定 90~92ms，无红色
 *
 * 本轮的算术：
 *   91ms / 296 行 / 31µs ≈ 10 帧
 *   而 LUT 的 TP 段第一字节是 0x0A —— 低半字节正好是 10。
 *   所以真正在驱动墨水的就是这一档。把它改小，就是直接砍时间。
 *
 * 扫描（每档交替刷 棋盘A / 棋盘B，各停 6s / 4s）：
 *   0x0A -> 10 帧  预期 ~91ms（基线）
 *   0x08 ->  8 帧  预期 ~73ms
 *   0x06 ->  6 帧  预期 ~55ms
 *   0x04 ->  4 帧  预期 ~37ms
 *   0x03 ->  3 帧  预期 ~27ms
 *   0x02 ->  2 帧  预期 ~18ms
 *
 * 判读：哪一档开始"发灰、棋盘糊在一起"，拐点就在它前一档。
 * 串口 115200，全程约 90 秒。
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

static uint8_t fbBW[FBB] __attribute__((aligned(4)));
static uint8_t fbRD[FBB] __attribute__((aligned(4)));

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

#define TP_IDX 60     // TP 段第一字节，低半字节 = 驱动帧数

static uint8_t lut[153];

static inline void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

static void noRed() { memset(fbRD, 0x00, FBB); }

static void checker(bool invert)
{
  memset(fbBW, 0xFF, FBB);
  noRed();
  for (int y = 30; y < 250; y++)
    for (int x = 6; x < 122; x++)
      if ((((x / 6) + (y / 6)) % 2 == 0) != invert) px(x, y, true);
}

static void white()
{
  memset(fbBW, 0xFF, FBB);
  noRed();
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

// tp = 驱动帧数（写到 TP 段第一字节的低半字节）
static uint32_t fast(uint8_t tp)
{
  memcpy(lut, LUT_BW, sizeof(lut));
  lut[TP_IDX] = (uint8_t)(lut[TP_IDX] & 0xF0) | (tp & 0x0F);
  display.epd2.expActivate(0xC0, 800);
  display.epd2.expWriteLut(lut, sizeof(lut));
  return act(0xC8, 20000);
}

static void step(uint8_t tp)
{
  Serial.printf("\n[TP=0x%02X] %u 帧\n", tp, tp);
  white();   uploadRaw();
  uint32_t a = fast(tp);
  Serial.printf("   全白   %lu ms\n", a);
  delay(1500);

  checker(false); uploadRaw();
  uint32_t b = fast(tp);
  Serial.printf("   棋盘A  %lu ms\n", b);
  Serial.printf("    >>> 看墨色，6s\n");
  delay(6000);

  checker(true); uploadRaw();
  uint32_t c = fast(tp);
  Serial.printf("   棋盘B  %lu ms\n", c);
  Serial.printf("    >>> 应与之互补，4s\n");
  delay(4000);
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("############ 3C TP SWEEP ############");

  display.init(115200, true, 10, false);

  Serial.println("\n[base] OTP 全白");
  white(); uploadRaw();
  Serial.printf("[base] %lu ms\n", act(0xF7, 40000));

  step(0x0A);
  step(0x08);
  step(0x06);
  step(0x04);
  step(0x03);
  step(0x02);

  Serial.println("\n[tail] OTP 恢复白");
  white(); uploadRaw();
  Serial.printf("[tail] %lu ms\n", act(0xF7, 40000));

  Serial.println("\n########## DONE ##########");
  Serial.println("把你觉得\"开始糊\"的那一档告诉我，拐点就在它前一档");
}

void loop()
{
  delay(60000);
  Serial.printf("[idle] heap=%u\n", ESP.getFreeHeap());
}
