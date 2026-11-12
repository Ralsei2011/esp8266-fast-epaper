/*
 * 三档波形同屏对比（A/B/C）
 *
 * 屏幕纵向切成三块，每块 98 行，图案完全相同，只换驱动波形：
 *   C 区 (196..293) : F7 基线（室温 OTP 波形，最慢最准）
 *   A 区 (  0.. 97) : D7 + 65℃（11.2s 档）
 *   B 区 ( 98..195) : D7 + 100℃（5.1s 档，已知发灰）
 *
 * 顺序：先刷 C（基线，建立参考），再刷 A，最后刷 B。
 *   → C 是参考，A/B 各自只被后续刷新轻微扰动，可比。
 *
 * 每块 98 行内的图案：
 *   0-13 纯黑 / 14-27 纯红 / 28-55 1px细线 / 56-69 纯白
 *   70-83 4x4 棋盘 / 84-97 1px 棋盘（最难，最容易翻车）
 */

#include <SPI.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W        128
#define REG_H    98
#define Y_A      0
#define Y_B      98
#define Y_C      196

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

// 区块内相对坐标 yy (0..97)
static bool bwBlack(uint16_t x, uint16_t yy)
{
  if (yy < 14)  return true;                       // 黑条
  if (yy < 28)  return false;                      // 红条（黑白平面=白）
  if (yy < 56)  return (yy % 4 == 0);              // 1px 细线
  if (yy < 70)  return false;                      // 白
  if (yy < 84)  return (((x / 4) + (yy / 4)) & 1); // 4x4 棋盘
  return ((x + yy) & 1);                           // 1px 棋盘
}
static bool isRed(uint16_t x, uint16_t yy)
{
  return (yy >= 14 && yy < 28);
}

uint32_t refreshRegion(uint16_t y0, uint16_t h, uint8_t mode, uint8_t temp)
{
  digitalWrite(PIN_RST, LOW);  delay(20);
  digitalWrite(PIN_RST, HIGH); delay(20);
  waitBusy();
  epdCmd(0x12); waitBusy();

  uint16_t gdr = y0 + h - 1;                 // 门线扫到区块底部
  epdCmd(0x01); epdDat(gdr & 0xFF); epdDat(gdr >> 8); epdDat(0x00);
  epdCmd(0x11); epdDat(0x03);
  epdCmd(0x3C); epdDat(0x05);
  epdCmd(0x18); epdDat(0x80);
  epdCmd(0x21); epdDat(0x00); epdDat(0x80);

  epdCmd(0x44); epdDat(0x00); epdDat((W - 1) / 8);
  epdCmd(0x45); epdDat(y0 & 0xFF); epdDat(y0 >> 8);
  epdDat((y0 + h - 1) & 0xFF); epdDat((y0 + h - 1) >> 8);
  epdCmd(0x4E); epdDat(0x00);
  epdCmd(0x4F); epdDat(y0 & 0xFF); epdDat(y0 >> 8);

  epdCmd(0x24);
  for (uint16_t i = 0; i < h; i++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++)
        if (!bwBlack(xb * 8 + b, i)) v |= (1 << (7 - b));
      epdDat(v);
    }

  epdCmd(0x26);
  for (uint16_t i = 0; i < h; i++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++)
        if (!isRed(xb * 8 + b, i)) v |= (1 << (7 - b));
      epdDat(v);
    }

  epdCmd(0x22); epdDat(0xF8); epdCmd(0x20); waitBusy();

  uint32_t t0 = millis();
  if (mode == 1) {
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
  Serial.println(F("#### 三档波形 A/B/C 对比 ####"));
  Serial.println(F("C=底部基线  A=顶部65C  B=中部100C"));

  Serial.print(F("[C] y196..293  F7 基线 ... "));
  Serial.printf("%lu ms\r\n", (unsigned long)refreshRegion(Y_C, REG_H, 0, 0));
  delay(2500);

  Serial.print(F("[A] y  0.. 97  D7 65C  ... "));
  Serial.printf("%lu ms\r\n", (unsigned long)refreshRegion(Y_A, REG_H, 1, 0x41));
  delay(2500);

  Serial.print(F("[B] y 98..195  D7 100C ... "));
  Serial.printf("%lu ms\r\n", (unsigned long)refreshRegion(Y_B, REG_H, 1, 0x64));

  Serial.println(F("DONE"));
  Serial.println(F("底部=基线 中间=100C(5.1s档)  顶部=65C(11.2s档)"));
}

void loop() { delay(1000); }
