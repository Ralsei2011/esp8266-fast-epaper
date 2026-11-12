/*
 * 温度覆写扫描（FastFull）——目标：把满屏刷从 19.4s 压下来
 *
 * 原理：SSD1680 的 OTP 里按温度区间存了不同的波形，高温区间的波形帧数少得多。
 *       0x1A 强制写温度寄存器，再 0x22=0xD7 让它在「不重新读传感器」的前提下
 *       用这个温度去挑 OTP 波形。
 *
 * 对照组设计：
 *   - 0xF7            : 基线（正常读传感器，室温）
 *   - 0xD7 + 100℃     : 极限快
 *   - 0xD7 +  80℃
 *   - 0xD7 +  65℃
 *   - 0xD7 +  50℃
 *   - 0xD7 +  10℃     : ★反向对照。如果它比基线更慢，说明温度覆写真的生效
 *
 * 每次刷新用完全相同的图（横向条纹 + 底部红块），保证时间可比。
 * 自动开跑，结果存 EEPROM。
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
#define EEP_MAGIC 0xA5A5

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
  uint32_t t0 = millis();
  delay(1);
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - t0 > 70000) { Serial.println(F("!! TIMEOUT")); break; }
    delay(1); yield();
  }
  return millis() - t0;
}

// ---------------- 变体表 ----------------
struct Var { uint8_t fast; uint8_t temp; const char *name; };
const Var vars[] = {
  { 0, 0x00, "BASE  F7  (读传感器)" },
  { 1, 0x64, "FAST  D7  100C"      },
  { 1, 0x50, "FAST  D7   80C"      },
  { 1, 0x41, "FAST  D7   65C"      },
  { 1, 0x32, "FAST  D7   50C"      },
  { 1, 0x0A, "FAST  D7   10C <反向对照>" },
};
const uint8_t varLen = sizeof(vars) / sizeof(vars[0]);

typedef struct { uint16_t magic; uint8_t n; uint32_t ms[8]; } Rec;
Rec rec;

uint32_t refresh(uint8_t fast, uint8_t temp)
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
    for (uint8_t xb = 0; xb < W / 8; xb++)
      epdDat((((y / 20) % 2 == 0) && (y < 240)) ? 0x00 : 0xFF);

  epdCmd(0x26);
  for (uint16_t y = 0; y < H; y++)
    epdDat((y >= 240) ? 0x00 : 0xFF);

  epdCmd(0x22); epdDat(0xF8); epdCmd(0x20); waitBusy();   // 上电

  uint32_t t0 = millis();
  if (fast) {
    epdCmd(0x1A); epdDat(temp);        // ★ 温度覆写
    epdCmd(0x22); epdDat(0xD7);
  } else {
    epdCmd(0x22); epdDat(0xF7);
  }
  epdCmd(0x20);
  waitBusy();
  uint32_t dt = millis() - t0;

  epdCmd(0x22); epdDat(0x83); epdCmd(0x20); waitBusy();   // 下电
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

  EEPROM.begin(512);
  EEPROM.get(EEP_ADDR, rec);
  if (rec.magic != EEP_MAGIC) { rec.magic = EEP_MAGIC; rec.n = 0; }

  Serial.println();
  Serial.println(F("#### 温度覆写扫描 (FastFull) ####"));
  Serial.printf("BUSY idle=%d\r\n", digitalRead(PIN_BUSY));

  for (uint8_t i = 0; i < varLen; i++) {
    Serial.printf("[%u/%u] %s ... ", i + 1, varLen, vars[i].name);
    uint32_t ms = refresh(vars[i].fast, vars[i].temp);
    Serial.printf("%lu ms\r\n", (unsigned long)ms);
    if (rec.n < 8) rec.ms[rec.n++] = ms;
    EEPROM.put(EEP_ADDR, rec); EEPROM.commit();
    delay(2500);
  }

  Serial.println(F("#### 结果 ####"));
  for (uint8_t i = 0; i < varLen; i++) {
    Serial.printf("  %-24s %s%lu ms\r\n", vars[i].name,
                  rec.ms[i] < 19400 ? " <- 更快 " : "        ",
                  (unsigned long)rec.ms[i]);
  }
  Serial.println(F("DONE"));
}

void loop() { delay(1000); }
