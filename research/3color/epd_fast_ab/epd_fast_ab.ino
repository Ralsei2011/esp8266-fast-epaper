/*
 * 快波形 A/B 同屏对比
 *
 * 上半屏（0..147 行）最后用 D7 + 100℃ 快波形刷（5.1s 档）
 * 下半屏（148..295 行）用 F7 基线波形刷（27.2s 档）
 * 两半的图案完全一样，直接比锐度/对比度/残影
 *
 * 流程：
 *   1) 整屏用 F7 刷一次          -> 建立基准
 *   2) 只刷上半 148 行, 用 D7+100C -> 上半变成"快波形"版本
 *   最后停在 A/B 对比画面
 */

#include <SPI.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W        128
#define H        296
#define HALF     148

SPISettings spi_settings(4000000, MSBFIRST, SPI_MODE0);

static inline void epdCmd(uint8_t c)
{
  SPI.beginTransaction(spi_settings);
  digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CS, HIGH); SPI.endTransaction();
}
static inline void epdDat(uint8_t d)
{
  SPI.beginTransaction(spi_settings);
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.transfer(d);
  digitalWrite(PIN_CS, HIGH); SPI.endTransaction();
}
static uint32_t waitBusy(void)
{
  uint32_t t0 = millis(); delay(1);
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - t0 > 70000) { Serial.println(F("!! TIMEOUT")); break; }
    delay(1); yield();
  }
  return millis() - t0;
}

/* 半屏图案：y 是半屏内坐标 0..147
 *  0.. 19  纯黑条
 * 20.. 39  纯红条
 * 40.. 79  1px 细线（每 4 行一条）—— 看锐度
 * 80.. 99  白
 *100..119  4x4 棋盘格        —— 看分辨率
 *120..147  1px 棋盘格        —— 最难的抖动图案，最容易发灰
 */
static bool bwBlack(uint16_t x, uint16_t y)
{
  uint16_t yy = y % HALF;
  if (yy < 20) return true;
  if (yy < 40) return false;                    // 红区，黑白平面为白
  if (yy < 80) return (yy % 4 == 0);
  if (yy < 100) return false;
  if (yy < 120) return (((x / 4) + (yy / 4)) & 1);
  return ((x + yy) & 1);
}
static bool isRed(uint16_t x, uint16_t y)
{
  uint16_t yy = y % HALF;
  return (yy >= 20 && yy < 40);
}

uint32_t refresh(uint16_t h, bool fast, uint8_t temp)
{
  digitalWrite(PIN_RST, LOW);  delay(20);
  digitalWrite(PIN_RST, HIGH); delay(20);
  waitBusy();
  epdCmd(0x12); waitBusy();

  uint16_t gdr = h - 1;
  epdCmd(0x01); epdDat(gdr & 0xFF); epdDat(gdr >> 8); epdDat(0x00);
  epdCmd(0x11); epdDat(0x03);
  epdCmd(0x3C); epdDat(0x05);
  epdCmd(0x18); epdDat(0x80);
  epdCmd(0x21); epdDat(0x00); epdDat(0x80);

  epdCmd(0x44); epdDat(0x00); epdDat((W - 1) / 8);
  epdCmd(0x45); epdDat(0x00); epdDat(0x00); epdDat((h - 1) & 0xFF); epdDat((h - 1) >> 8);
  epdCmd(0x4E); epdDat(0x00);
  epdCmd(0x4F); epdDat(0x00); epdDat(0x00);

  epdCmd(0x24);
  for (uint16_t y = 0; y < h; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++)
        if (!bwBlack(xb * 8 + b, y)) v |= (1 << (7 - b));   // 1=白 0=黑
      epdDat(v);
    }

  epdCmd(0x26);
  for (uint16_t y = 0; y < h; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++)
        if (!isRed(xb * 8 + b, y)) v |= (1 << (7 - b));     // 1=非红 0=红
      epdDat(v);
    }

  epdCmd(0x22); epdDat(0xF8); epdCmd(0x20); waitBusy();

  uint32_t t0 = millis();
  if (fast) {
    epdCmd(0x1A); epdDat(temp);
    epdCmd(0x22); epdDat(0xD7);
  } else {
    epdCmd(0x22); epdDat(0xF7);
  }
  epdCmd(0x20);
  waitBusy();
  uint32_t dt = millis() - t0;

  epdCmd(0x22); epdDat(0x83); epdCmd(0x20); waitBusy();
  return dt;
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  SPI.begin();
  SPI.setFrequency(4000000);

  Serial.println();
  Serial.println(F("#### 快波形 A/B 对比 ####"));

  Serial.print(F("[1] 整屏 F7 基线 ... "));
  uint32_t a = refresh(H, false, 0);
  Serial.printf("%lu ms\r\n", (unsigned long)a);
  delay(4000);

  Serial.print(F("[2] 上半 148 行 D7+100C ... "));
  uint32_t b = refresh(HALF, true, 0x64);
  Serial.printf("%lu ms\r\n", (unsigned long)b);

  Serial.println(F("DONE  上半=快波形  下半=基线波形"));
  Serial.println(F("对比：上半的 1px 细线/棋盘格 是不是比下半糊？黑条够不够黑？红条正不正？"));
}

void loop() { delay(1000); }
