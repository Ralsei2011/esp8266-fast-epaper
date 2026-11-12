/*
 * 区域局刷「衰减」压力测试
 *
 * 目的：量化「刷带子时下方不动区域会变淡」这个副作用
 *   1) 连续 10 次 h=56 区域刷，每次内容都变（进度条 + 棋盘格翻转）
 *   2) 最后做一次 h=296 满屏刷，看能否完全恢复
 *
 * 观察点：第 1/5/10 次时，下方参考图（每20行黑横带 + 底部纯红）还剩几分
 *         最后全刷后是否 100% 复原
 *
 * 自动开跑，不需要按键。结果写 EEPROM，开机可回读。
 */

#include <SPI.h>
#include <EEPROM.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W        128
#define H_FULL   296
#define BAND     56
#define ITERS    10

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
static uint32_t waitBusy(const char *tag)
{
  uint32_t t0 = millis();
  delay(1);
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - t0 > 35000) { Serial.print(F("!! TIMEOUT ")); Serial.println(tag); break; }
    delay(1); yield();
  }
  return millis() - t0;
}

// ---- 内容生成器：mode 0=参考图(全屏) 1=进度带(第 i 次) ----
static uint8_t pixBw(uint16_t x, uint16_t y, uint8_t mode, uint8_t i)
{
  if (mode == 0) return (((y / 20) % 2 == 0) && (y < 240)) ? 0 : 1;   // 0=黑 1=白
  // 进度带：上半 28 行是进度条，下半是棋盘格
  if (y < 28) return (x < (uint16_t)(i + 1) * 12) ? 0 : 1;
  return (((x / 8) + (y / 8) + (i & 1)) & 1) ? 1 : 0;
}
static uint8_t pixRed(uint16_t x, uint16_t y, uint8_t mode, uint8_t i)
{
  if (mode == 0) return (y >= 240) ? 0 : 1;    // 0=红 1=非红
  if (y < 28) return 1;
  // 棋盘格里给中间 16 列加红，方便看红色是否也衰减
  return (x >= 56 && x < 72) ? 0 : 1;
}

uint32_t refresh(uint16_t h, uint8_t mode, uint8_t i)
{
  digitalWrite(PIN_RST, LOW);  delay(20);
  digitalWrite(PIN_RST, HIGH); delay(20);
  waitBusy("hwreset");
  epdCmd(0x12); waitBusy("swreset");

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
        if (pixBw(xb * 8 + b, y, mode, i)) v |= (1 << (7 - b));
      epdDat(v);
    }

  epdCmd(0x26);
  for (uint16_t y = 0; y < h; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++)
        if (pixRed(xb * 8 + b, y, mode, i)) v |= (1 << (7 - b));
      epdDat(v);
    }

  epdCmd(0x22); epdDat(0xF8); epdCmd(0x20); waitBusy("poweron");

  epdCmd(0x22); epdDat(0xF7); epdCmd(0x20);
  uint32_t dt = waitBusy("update");

  epdCmd(0x22); epdDat(0x83); epdCmd(0x20); waitBusy("poweroff");
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
  Serial.println(F("###### 区域局刷衰减压力测试 ######"));
  Serial.printf("BUSY idle=%d  heap=%u\r\n", digitalRead(PIN_BUSY), ESP.getFreeHeap());

  Serial.println(F("[STEP 0] 建立全屏参考图 (h=296)"));
  uint32_t t = refresh(296, 0, 0);
  Serial.printf(">>> ref h=296  %lu ms\r\n", (unsigned long)t);
  delay(3000);

  for (uint8_t i = 0; i < ITERS; i++) {
    t = refresh(BAND, 1, i);
    Serial.printf(">>> part #%u  h=%u  %lu ms\r\n", i + 1, (unsigned) BAND, (unsigned long)t);
  }

  Serial.println(F("[STEP X] 最后全刷恢复 (h=296)"));
  t = refresh(296, 0, 0);
  Serial.printf(">>> restore h=296  %lu ms\r\n", (unsigned long)t);

  Serial.println(F("###### DONE ######"));
  Serial.println(F("看屏幕：下方横带/红块在第10次区域刷后还剩几分？最后全刷后是否完全复原？"));
}

void loop()
{
  delay(1000);
}
