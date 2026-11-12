/*
 * 平面极性诊断 —— 整屏纯色填充，每个模式只填一种组合
 *
 * SSD1680 三色屏有两个平面：
 *   0x24 (BW 平面) : 1=白  0=黑
 *   0x26 (RED 平面): 1=非红 0=红
 * 但这只是「约定」，实际四象限要实测：
 *
 *   mode 0 : BW=0xFF  RED=0xFF   → 期望 纯白
 *   mode 1 : BW=0x00  RED=0xFF   → 期望 纯黑
 *   mode 2 : BW=0xFF  RED=0x00   → 期望 纯红
 *   mode 3 : BW=0x00  RED=0x00   → 第四象限（未知，观察）
 *   mode 4 : BW=0xFF  RED=0xFF   → 再擦白一次，确认可恢复
 *
 * 用基线波形 F7（最可靠），每个模式都在屏幕中央画 4 个小白点，
 * 用来确认「哪一面朝上」和「上下有没有翻转」：
 *   mode>=0 时，x=8/40/72/104, y=148 处各画一个 8x8 白点
 *   （因为底色可能是白，白点只在深色底上看得见；mode0 的白底上看不见属正常）
 *
 * 切下一档：电池开关 OFF 2 秒 → ON
 */

#include <SPI.h>
#include <EEPROM.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W        128
#define H        296

#define EEP_ADDR  0
#define EEP_MAGIC 0xB400

SPISettings spi_settings(4000000, MSBFIRST, SPI_MODE0);

static inline void epdCmd(uint8_t c)
{
  SPI.beginTransaction(spi_settings);
  digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c); digitalWrite(PIN_CS, HIGH); SPI.endTransaction();
}
static inline void epdDat(uint8_t d)
{
  SPI.beginTransaction(spi_settings);
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  SPI.transfer(d); digitalWrite(PIN_CS, HIGH); SPI.endTransaction();
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

// 中央定位点：4 个 8x8 的白方块 + 左上角一个黑角标
static bool markerWhite(uint16_t x, uint16_t y)
{
  if (y < 144 || y >= 152) return false;
  return (x % 32) < 8;
}
static bool markerBlack(uint16_t x, uint16_t y)
{
  return (x < 16 && y < 16);   // 左上角黑角标，确认方向
}

void fill(uint8_t bw, uint8_t red, bool withMarks)
{
  digitalWrite(PIN_RST, LOW);  delay(20);
  digitalWrite(PIN_RST, HIGH); delay(20);
  waitBusy();
  epdCmd(0x12); waitBusy();

  epdCmd(0x01); epdDat(0x27); epdDat(0x01); epdDat(0x00);
  epdCmd(0x11); epdDat(0x03);
  epdCmd(0x3C); epdDat(0x05);
  epdCmd(0x18); epdDat(0x80);
  epdCmd(0x21); epdDat(0x00); epdDat(0x80);

  epdCmd(0x44); epdDat(0x00); epdDat((W - 1) / 8);
  epdCmd(0x45); epdDat(0x00); epdDat(0x00); epdDat(0x27); epdDat(0x01);
  epdCmd(0x4E); epdDat(0x00);
  epdCmd(0x4F); epdDat(0x00); epdDat(0x00);

  epdCmd(0x24);
  for (uint16_t y = 0; y < H; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = bw;
      if (withMarks) {
        for (uint8_t b = 0; b < 8; b++) {
          uint16_t x = xb * 8 + b;
          if (markerWhite(x, y))      v |=  (1 << (7 - b));   // BW=1 -> 白
          else if (markerBlack(x, y)) v &= ~(1 << (7 - b));   // BW=0 -> 黑
        }
      }
      epdDat(v);
    }

  epdCmd(0x26);
  for (uint16_t y = 0; y < H; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = red;
      if (withMarks) {
        for (uint8_t b = 0; b < 8; b++) {
          uint16_t x = xb * 8 + b;
          if (markerWhite(x, y)) v |= (1 << (7 - b));   // 非红，露出白/黑
        }
      }
      epdDat(v);
    }

  epdCmd(0x22); epdDat(0xF8); epdCmd(0x20); waitBusy();
  epdCmd(0x22); epdDat(0xF7); epdCmd(0x20);
  waitBusy();
  epdCmd(0x22); epdDat(0x83); epdCmd(0x20); waitBusy();
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

  EEPROM.begin(512);
  uint16_t v = 0;
  EEPROM.get(EEP_ADDR, v);
  if ((v & 0xFF00) != EEP_MAGIC) v = EEP_MAGIC;
  uint8_t mode = v & 0x00FF;

  Serial.println();
  Serial.printf("#### 平面极性诊断  mode=%u ####\r\n", mode);

  uint8_t bw = 0xFF, red = 0xFF;
  const char *name = "";
  switch (mode) {
    case 0: bw = 0xFF; red = 0xFF; name = "BW=FF RED=FF  期望纯白"; break;
    case 1: bw = 0x00; red = 0xFF; name = "BW=00 RED=FF  期望纯黑"; break;
    case 2: bw = 0xFF; red = 0x00; name = "BW=FF RED=00  期望纯红"; break;
    case 3: bw = 0x00; red = 0x00; name = "BW=00 RED=00  第四象限"; break;
    case 4: bw = 0xFF; red = 0xFF; name = "BW=FF RED=FF  再擦白"; break;
  }
  Serial.printf("[fill] %s\r\n", name);

  uint32_t t0 = millis();
  fill(bw, red, true);
  Serial.printf(">>> %lu ms\r\n", (unsigned long)(millis() - t0));
  Serial.println(F("DONE — 关电池开关 2 秒再开 → 下一模式"));

  mode = (mode + 1) % 5;
  EEPROM.put(EEP_ADDR, (uint16_t)(EEP_MAGIC | mode));
  EEPROM.commit();
}

void loop() { delay(1000); }
