/*
 * 波形质量单档测试（一次一档，带屏上标签，测前先擦白）
 *
 * 每次开机：
 *   1) 先用基线波形把整屏刷成纯白（彻底擦掉上一轮残留）  ~27s
 *   2) 再用当前档位的波形，画「标签 + 测试图案」全屏刷一次
 *   3) 档位号 +1 存 EEPROM，然后停住
 *
 * 切下一档：把电池开关 OFF 等 2 秒再 ON（不要按按键）
 *
 * 档位：
 *   0 = BASE   基线 F7（读传感器）        标签：一个大空心方框
 *   1 = 65C    D7 + 0x41                标签：数字 65
 *   2 = 100C   D7 + 0x64                标签：数字 100
 *   3 = 100Cx2 同样波形连打两次           标签：数字 100 + 右下角一个小方块
 *
 * 图案区（每档完全相同）：
 *   黑条 / 红条 / 1px 细线 / 白 / 4x4 棋盘 / 1px 棋盘
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
#define EEP_MAGIC 0xC300

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

// ================= 7 段数码管 =================
// 段位: bit0=a bit1=b bit2=c bit3=d bit4=e bit5=f bit6=g
static const uint8_t SEG_DIGIT[10] = {
  0x3F, // 0
  0x06, // 1
  0x5B, // 2
  0x4F, // 3
  0x66, // 4
  0x6D, // 5
  0x7D, // 6
  0x07, // 7
  0x7F, // 8
  0x6F  // 9
};

// 判断数字内部某点是否为黑（ox,oy 为数字左上角，w/h 为数字尺寸）
static bool digitBlack(int16_t px, int16_t py, int16_t ox, int16_t oy,
                       int16_t w, int16_t h, uint8_t mask)
{
  int16_t x = px - ox, y = py - oy;
  if (x < 0 || y < 0 || x >= w || y >= h) return false;
  int16_t t = w / 5;                 // 笔画粗细
  if (t < 2) t = 2;
  int16_t half = h / 2;
  struct Seg { int16_t x, y, w, h; uint8_t bit; };
  Seg segs[7] = {
    { 0,           0,              w, t,        0 }, // a
    { w - t,       0,              t, half,     1 }, // b
    { w - t,       half,           t, h - half, 2 }, // c
    { 0,           h - t,          w, t,        3 }, // d
    { 0,           half,           t, h - half, 4 }, // e
    { 0,           0,              t, half,     5 }, // f
    { 0,           half - t / 2,   w, t,        6 }, // g
  };
  for (uint8_t i = 0; i < 7; i++) {
    if (!(mask & (1 << segs[i].bit))) continue;
    if (x >= segs[i].x && x < segs[i].x + segs[i].w &&
        y >= segs[i].y && y < segs[i].y + segs[i].h) return true;
  }
  return false;
}

// 在 x,y 处画一个数（右对齐不算，直接左起），dw/dh 单字尺寸
static bool numberBlack(int16_t px, int16_t py, int16_t x0, int16_t y0,
                        int16_t dw, int16_t dh, uint16_t value, uint8_t digits)
{
  int16_t step = dw + dw / 2;
  for (uint8_t i = 0; i < digits; i++) {
    uint16_t div = 1;
    for (uint8_t k = i + 1; k < digits; k++) div *= 10;
    uint8_t d = (value / div) % 10;
    if (digitBlack(px, py, x0 + i * step, y0, dw, dh, SEG_DIGIT[d])) return true;
  }
  return false;
}

// ================= 图案 =================
#define PAT_Y0   56
#define PAT_H    200

static bool patternBlack(uint16_t x, uint16_t y)
{
  if (y < PAT_Y0 || y >= PAT_Y0 + PAT_H) return false;
  uint16_t yy = y - PAT_Y0;
  if (yy < 40)  return true;                        // 黑条
  if (yy < 80)  return false;                       // 红条
  if (yy < 120) return (yy % 4 == 0);               // 1px 细线
  if (yy < 140) return false;                       // 白
  if (yy < 170) return (((x / 4) + (yy / 4)) & 1);  // 4x4 棋盘
  return ((x + yy) & 1);                            // 1px 棋盘
}
static bool patternRed(uint16_t x, uint16_t y)
{
  if (y < PAT_Y0 || y >= PAT_Y0 + PAT_H) return false;
  uint16_t yy = y - PAT_Y0;
  return (yy >= 40 && yy < 80);
}

// 标签
static bool labelBlack(uint16_t x, uint16_t y, uint8_t variant)
{
  if (y < 12 || y >= 44) return false;
  if (variant == 0) {
    // BASE：空心方框
    if (x >= 8 && x < 108) {
      if (y < 16 || y >= 40) return true;
      if (x < 12 || x >= 104) return true;
    }
    return false;
  }
  uint16_t val = (variant == 1) ? 65 : 100;
  uint8_t digits = (variant == 1) ? 2 : 3;
  if (numberBlack(x, y, 8, 12, 16, 32, val, digits)) return true;
  // variant 3 额外在右下角点一个小实心方块
  if (variant == 3 && x >= 100 && x < 116 && y >= 26 && y < 42) return true;
  return false;
}

// ================= 刷新 =================
// mode: 0=基线F7  1=D7+temp
uint32_t refreshFull(uint8_t mode, uint8_t temp, uint8_t variant,
                     bool drawPattern, uint8_t passes)
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
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++) {
        uint16_t x = xb * 8 + b;
        bool black = false;
        if (drawPattern) black = patternBlack(x, y) || labelBlack(x, y, variant);
        if (!black) v |= (1 << (7 - b));
      }
      epdDat(v);
    }

  epdCmd(0x26);
  for (uint16_t y = 0; y < H; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++) {
        uint16_t x = xb * 8 + b;
        bool red = drawPattern ? patternRed(x, y) : false;
        if (!red) v |= (1 << (7 - b));
      }
      epdDat(v);
    }

  uint32_t total = 0;
  for (uint8_t p = 0; p < passes; p++) {
    epdCmd(0x22); epdDat(0xF8); epdCmd(0x20); waitBusy();   // 上电
    uint32_t t0 = millis();
    if (mode == 1) { epdCmd(0x1A); epdDat(temp); epdCmd(0x22); epdDat(0xD7); }
    else           { epdCmd(0x22); epdDat(0xF7); }
    epdCmd(0x20);
    waitBusy();
    total += millis() - t0;
    epdCmd(0x22); epdDat(0x83); epdCmd(0x20); waitBusy();   // 下电
  }
  return total;
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
  uint8_t variant = v & 0x00FF;

  Serial.println();
  Serial.printf("#### 单档测试  variant=%u ####\r\n", variant);

  // 1) 擦白
  Serial.print(F("[擦白] 基线 F7 全白 ... "));
  uint32_t e = refreshFull(0, 0, 0, false, 1);
  Serial.printf("%lu ms\r\n", (unsigned long)e);
  delay(1500);

  // 2) 本档
  uint8_t mode = (variant == 0) ? 0 : 1;
  uint8_t temp = (variant == 1) ? 0x41 : 0x64;
  uint8_t passes = (variant == 3) ? 2 : 1;

  Serial.printf("[测试] variant=%u mode=%u temp=0x%02X passes=%u ... ",
                variant, mode, temp, passes);
  uint32_t t = refreshFull(mode, temp, variant, true, passes);
  Serial.printf("%lu ms\r\n", (unsigned long)t);
  Serial.println(F("DONE — 关掉电池开关 2 秒再打开 → 切下一档"));

  // 3) 档位前进
  variant = (variant + 1) % 4;
  EEPROM.put(EEP_ADDR, (uint16_t)(EEP_MAGIC | variant));
  EEPROM.commit();
}

void loop() { delay(1000); }
