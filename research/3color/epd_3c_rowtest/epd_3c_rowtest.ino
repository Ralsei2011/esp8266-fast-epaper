/*
 * 三色屏纯黑白快刷 —— 第七轮：缩短扫描行数
 * ==========================================================================
 * 第六轮已确认：92ms 来自 OTP 的 Mode 2 波形，与 0x32 无关。
 *
 * 92ms 是 OTP 固化的，改不了相数，但可以改"扫多少行"：
 *   T = 行数 × 每行时间，每行时间是个常数。
 *   0x01 (Driver Output Control) 的 GDR 决定扫描门线数，
 *   RAM 窗口（0x44/0x45/0x4e/0x4f）决定写多少行数据。
 *   两者必须一致，否则地址映射和扫描范围对不上，画面会错位。
 *
 * 本轮的算术预期（以全屏 92ms / 296 行为基准，每行约 311µs）：
 *   296 行 -> 92 ms
 *   148 行 -> 46 ms
 *    96 行 -> 30 ms
 *    64 行 -> 20 ms
 *    32 行 -> 10 ms
 *
 * 每档只在前 h 行画棋盘，屏上会看到"上方 h 行变化、下方保持"，边界就是 h。
 * 串口 115200，全程约 60 秒。
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

static inline void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

// 只在前 h 行画棋盘，其余保持白
static void checkerRows(uint16_t h, bool invert)
{
  memset(fbBW, 0xFF, FBB);
  memset(fbRD, 0x00, FBB);
  for (int y = 4; y < (int)h; y++)
    for (int x = 6; x < 122; x++)
      if ((((x / 6) + (y / 6)) % 2 == 0) != invert) px(x, y, true);
}

static void fullWhite()
{
  memset(fbBW, 0xFF, FBB);
  memset(fbRD, 0x00, FBB);
}

// 写整屏两平面（用于 OTP 全刷）
static void uploadFull()
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

// GDR：0x01 + 3 字节，有效门线数 = h
static void setGdr(uint16_t h)
{
  uint8_t b[3];
  uint16_t g = h - 1;
  b[0] = g & 0xFF;
  b[1] = (g >> 8) & 0x01;
  b[2] = 0x00;
  display.epd2.expWriteRam(0x01, b, 3);
}

// 只刷前 h 行：GDR + RAM 窗口都设成 h，数据也只传 h 行
static uint32_t fastRows(uint16_t h)
{
  setGdr(h);
  display.epd2.expSetRamArea(0, 0, W, h);
  display.epd2.expWriteRam(0x24, fbBW, (uint32_t)(W / 8) * h);
  display.epd2.expWriteRam(0x26, fbRD, (uint32_t)(W / 8) * h);
  display.epd2.expActivate(0xC0, 800);
  return act(0xC8, 20000);
}

static void step(uint16_t h, bool invert)
{
  checkerRows(h, invert);
  uint32_t t = fastRows(h);
  Serial.printf("[%3u 行] %lu ms   理论 %.0f ms\n", h, t, 92.0f * h / 296.0f);
  Serial.printf("    >>> 屏上应只有上方 %u 行是棋盘，6s\n", h);
  delay(6000);
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("############ 3C ROW SWEEP ############");

  display.init(115200, true, 10, false);

  Serial.println("\n[base] OTP 全白");
  fullWhite(); uploadFull();
  Serial.printf("[base] %lu ms\n", act(0xF7, 40000));

  step(296, false);
  step(148, true);
  step(96, false);
  step(64, true);
  step(32, false);

  Serial.println("\n[tail] 恢复全屏白");
  fullWhite(); uploadFull();
  setGdr(H);
  Serial.printf("[tail] %lu ms\n", act(0xF7, 40000));

  Serial.println("\n########## DONE ##########");
  Serial.println("若耗时按行数线性下降，就可按需选行数换帧率");
}

void loop()
{
  delay(60000);
  Serial.printf("[idle] heap=%u\n", ESP.getFreeHeap());
}
