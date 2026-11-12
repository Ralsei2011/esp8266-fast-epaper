/*
 * 三色屏纯黑白快刷 —— 第六轮：0x32 到底有没有用？
 * ==========================================================================
 * 第五轮的意外：把 LUT 的 TP 字节从 0x0A 一路改到 0x02（10 帧 -> 2 帧），
 * 六档耗时全部是 92ms，一点没变。说明 92ms 不是那张表决定的。
 *
 * 于是有个更简单的解释：
 *   SSD1680 三色的 OTP 里可能本来就存了两套波形
 *     Mode 1 -> 三色全刷（27.2s，但要推红）
 *     Mode 2 -> 黑白快速波形（0xC8 走这套，92ms）
 *   如果成立，自定义 LUT 完全是多余的，只要「红平面全非红 + 0xC8」。
 *
 * 本轮四组对照，直接判死：
 *   P2  完全不写 0x32，直接 0xC8        -> 若仍 92ms 且画面对 = OTP 自带 Mode2
 *   P3  0x32 全写 0x00，再 0xC8         -> 若变成 0ms/无画面 = 0x32 生效且覆盖了默认
 *   P4  0x32 全写 0xFF，再 0xC8         -> 若明显变慢/花屏 = 0x32 生效
 *   P5  0x32 写原表，再 0xC8            -> 复现 92ms
 *
 * 串口 115200，全程约 80 秒。
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

static uint8_t lut[153];

static inline void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

static void checker(bool invert)
{
  memset(fbBW, 0xFF, FBB);
  memset(fbRD, 0x00, FBB);       // RED 位 1=红，要黑白必须全 0
  for (int y = 30; y < 250; y++)
    for (int x = 6; x < 122; x++)
      if ((((x / 6) + (y / 6)) % 2 == 0) != invert) px(x, y, true);
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

// 不写 0x32，只上电 + 0xC8
static uint32_t fastNoLut()
{
  display.epd2.expActivate(0xC0, 800);
  return act(0xC8, 20000);
}

// 写指定 LUT 再 0xC8
static uint32_t fastWithLut(const uint8_t *table)
{
  display.epd2.expActivate(0xC0, 800);
  display.epd2.expWriteLut(table, 153);
  return act(0xC8, 20000);
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("############ 3C LUT EFFECT TEST ############");

  display.init(115200, true, 10, false);

  // P1 基线
  Serial.println("\n[P1] OTP 全白（基线，预期 ~5s）");
  memset(fbBW, 0xFF, FBB); memset(fbRD, 0x00, FBB); uploadRaw();
  Serial.printf("[P1] %lu ms\n", act(0xF7, 40000));

  // P2 不写 LUT
  Serial.println("\n[P2] 不写 0x32，直接 0x22=0xC8");
  checker(false); uploadRaw();
  uint32_t t2 = fastNoLut();
  Serial.printf("[P2] %lu ms  -> 若 ~92ms 且棋盘正确 = OTP 自带 Mode2 快刷\n", t2);
  Serial.println("    >>> 看屏 8s");
  delay(8000);

  // P3 全 0x00
  Serial.println("\n[P3] 0x32 全写 0x00，再 0xC8");
  checker(true); uploadRaw();
  memset(lut, 0x00, sizeof(lut));
  uint32_t t3 = fastWithLut(lut);
  Serial.printf("[P3] %lu ms  -> 若 0ms 或画面不动 = 0x32 真的覆盖了默认波形\n", t3);
  Serial.println("    >>> 看屏 8s");
  delay(8000);

  // P4 全 0xFF
  Serial.println("\n[P4] 0x32 全写 0xFF，再 0xC8");
  checker(false); uploadRaw();
  memset(lut, 0xFF, sizeof(lut));
  uint32_t t4 = fastWithLut(lut);
  Serial.printf("[P4] %lu ms  -> 若明显变慢/花屏 = 0x32 生效\n", t4);
  Serial.println("    >>> 看屏 8s");
  delay(8000);

  // P5 原表
  Serial.println("\n[P5] 0x32 写原表，再 0xC8");
  checker(true); uploadRaw();
  memcpy(lut, LUT_BW, sizeof(lut));
  uint32_t t5 = fastWithLut(lut);
  Serial.printf("[P5] %lu ms  -> 复现 92ms\n", t5);
  Serial.println("    >>> 看屏 8s");
  delay(8000);

  // P6 清理
  Serial.println("\n[P6] OTP 恢复白");
  memset(fbBW, 0xFF, FBB); memset(fbRD, 0x00, FBB); uploadRaw();
  Serial.printf("[P6] %lu ms\n", act(0xF7, 40000));

  Serial.println("\n########## DONE ##########");
  Serial.printf("P2无LUT=%lu  P3全0=%lu  P4全FF=%lu  P5原表=%lu\n", t2, t3, t4, t5);
  Serial.println("若 P2≈P5≈92ms，说明 0x32 无关，92ms 来自 OTP 的 Mode2");
}

void loop()
{
  delay(60000);
  Serial.printf("[idle] heap=%u\n", ESP.getFreeHeap());
}
