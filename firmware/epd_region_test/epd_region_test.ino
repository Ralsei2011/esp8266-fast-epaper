/*
 * 区域局刷验证：缩小门线数（0x01）+ 缩小 RAM 窗口（0x44/0x45），
 * 用三色屏标准的 0x22=0xF7 全刷波形，看刷新时间是否随门线数线性下降。
 *
 * 关键假设：SSD1680 的单帧时间 ∝ 门线数，所以只刷顶部 H 行时
 * 总时间 ≈ 27.2s × H / 296。若成立，56 行 ≈ 5s，这就是真正的"区域局刷"。
 *
 * 硬件：CS=15 DC=0 RST=2 BUSY=4（BUSY 高电平有效），按键=GPIO5（右键，低有效）
 * 用法：每次按右键 → 依次跑 h=296(基线) / 56 / 100 / 160，循环。
 *       结果写串口 + 存 EEPROM，开机时把历史记录全打出来（防串口掉线丢数据）。
 */

#include <SPI.h>
#include <EEPROM.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4
#define PIN_KEY   5

#define W        128
#define H_FULL   296
#define EEP_ADDR 0
#define EEP_MAGIC 0x5A5A

SPISettings spi_settings(4000000, MSBFIRST, SPI_MODE0);

typedef struct {
  uint16_t magic;
  uint8_t  count;
  uint8_t  rsv;
  uint16_t h[8];
  uint32_t ms[8];
} Rec;
Rec rec;

// ---------------- 底层 SPI ----------------
static inline void epdCmd(uint8_t c)
{
  SPI.beginTransaction(spi_settings);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

static inline void epdDat(uint8_t d)
{
  SPI.beginTransaction(spi_settings);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(d);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

// BUSY 高 = 忙（与 GxEPD2_290_C90c 的 busy_level 一致）
static uint32_t waitBusy(const char *tag)
{
  uint32_t t0 = millis();
  delay(1);
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - t0 > 35000) {
      Serial.print(F("!! BUSY TIMEOUT @ "));
      Serial.println(tag);
      break;
    }
    delay(1);
    yield();
  }
  return millis() - t0;
}

// ---------------- 图案 ----------------
// 竖三分：左黑 / 中红 / 右白，带底部 1px 黑线
// 0x24(BW 平面): 1=白 0=黑   0x26(RED 平面): 1=非红 0=红
static uint8_t bwByte(uint8_t xb, uint16_t y, uint16_t h)
{
  uint8_t v = 0;
  for (uint8_t b = 0; b < 8; b++) {
    uint16_t x = xb * 8 + b;
    bool black;
    if (y == h - 1)          black = true;                 // 带子底边
    else if (x < 42)         black = true;                 // 左：黑
    else                     black = false;                // 中右：非黑
    if (!black) v |= (1 << (7 - b));
  }
  return v;
}

static uint8_t redByte(uint8_t xb, uint16_t y, uint16_t h)
{
  uint8_t v = 0;
  for (uint8_t b = 0; b < 8; b++) {
    uint16_t x = xb * 8 + b;
    bool isRed = (y != h - 1) && (x >= 42 && x < 85);
    if (!isRed) v |= (1 << (7 - b));
  }
  return v;
}

// 全屏参考图（只在 h=296 时用）：每 20 行一条黑横带 + 底部 56 行纯红
static uint8_t refBwByte(uint8_t xb, uint16_t y)
{
  bool black = ((y / 20) % 2 == 0) && (y < 240);
  if (black) return 0x00;
  return 0xFF;
}

static uint8_t refRedByte(uint8_t xb, uint16_t y)
{
  return (y >= 240) ? 0x00 : 0xFF;   // 底部纯红
}

// ---------------- 一次区域刷新 ----------------
uint32_t refreshRegion(uint16_t h, bool useRef)
{
  // 硬复位
  digitalWrite(PIN_RST, LOW);
  delay(20);
  digitalWrite(PIN_RST, HIGH);
  delay(20);
  waitBusy("hwreset");

  epdCmd(0x12);                       // 软复位
  waitBusy("swreset");

  uint16_t gdr = h - 1;               // ★ 核心：门线数 = h
  epdCmd(0x01);                       // Driver output control
  epdDat(gdr & 0xFF);
  epdDat(gdr >> 8);
  epdDat(0x00);
  epdCmd(0x11); epdDat(0x03);         // 数据进入模式：X 增后 Y 增
  epdCmd(0x3C); epdDat(0x05);         // 边框波形
  epdCmd(0x18); epdDat(0x80);         // 内置温度传感器
  epdCmd(0x21); epdDat(0x00); epdDat(0x80);

  // RAM 窗口：纵向只到 h-1
  epdCmd(0x44); epdDat(0x00); epdDat((W - 1) / 8);
  epdCmd(0x45); epdDat(0x00); epdDat(0x00);
  epdDat((h - 1) & 0xFF); epdDat((h - 1) >> 8);
  epdCmd(0x4E); epdDat(0x00);
  epdCmd(0x4F); epdDat(0x00); epdDat(0x00);

  epdCmd(0x24);                       // 黑白平面
  for (uint16_t y = 0; y < h; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++)
      epdDat(useRef ? refBwByte(xb, y) : bwByte(xb, y, h));

  epdCmd(0x26);                       // 红色平面
  for (uint16_t y = 0; y < h; y++)
    for (uint8_t xb = 0; xb < W / 8; xb++)
      epdDat(useRef ? refRedByte(xb, y) : redByte(xb, y, h));

  epdCmd(0x22); epdDat(0xF8);         // 上电
  epdCmd(0x20);
  waitBusy("poweron");

  epdCmd(0x22); epdDat(0xF7);         // ★ 全刷波形（三色唯一正式路径）
  epdCmd(0x20);
  uint32_t t0 = millis();
  uint32_t dt = waitBusy("update");

  epdCmd(0x22); epdDat(0x83);         // 下电
  epdCmd(0x20);
  waitBusy("poweroff");

  return dt;
}

// ---------------- 记录 ----------------
void saveRec()
{
  EEPROM.put(EEP_ADDR, rec);
  EEPROM.commit();
}

void dumpRec()
{
  Serial.println(F("--- 历史记录（EEPROM）---"));
  for (uint8_t i = 0; i < rec.count && i < 8; i++) {
    Serial.printf("  #%u  h=%-3u  %5lu ms  (线性外推 h=296 应 %.0f ms)\r\n",
                  i, rec.h[i], (unsigned long)rec.ms[i],
                  rec.h[i] ? (double)rec.ms[i] * 296.0 / rec.h[i] : 0.0);
  }
  Serial.println(F("--------------------------"));
}

void addRec(uint16_t h, uint32_t ms)
{
  if (rec.count >= 8) {
    memmove(&rec.h[0], &rec.h[1], 7 * sizeof(uint16_t));
    memmove(&rec.ms[0], &rec.ms[1], 7 * sizeof(uint32_t));
    rec.count = 7;
  }
  rec.h[rec.count] = h;
  rec.ms[rec.count] = ms;
  rec.count++;
  saveRec();
}

// ---------------- 主流程 ----------------
uint8_t step = 0;
const uint16_t plan[] = { 296, 56, 100, 160 };
const uint8_t planLen = sizeof(plan) / sizeof(plan[0]);

void runStep()
{
  uint16_t h = plan[step % planLen];
  bool useRef = (h == 296);
  step++;

  Serial.printf("\r\n===== TEST h=%u (%s) =====\r\n", h,
                useRef ? "全屏参考图" : "区域带");
  Serial.printf("heap before: %u\r\n", ESP.getFreeHeap());

  uint32_t ms = refreshRegion(h, useRef);

  Serial.printf(">>> h=%u  %lu ms   (27.2s x %u/296 = %.0f ms 理论值)\r\n",
                h, (unsigned long)ms, h, 27200.0 * h / 296.0);
  Serial.printf("heap after : %u\r\n", ESP.getFreeHeap());
  addRec(h, ms);
  dumpRec();
}

void setup()
{
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_CS, OUTPUT);   digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);   digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT);  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_KEY, INPUT_PULLUP);

  SPI.begin();
  SPI.setFrequency(4000000);

  EEPROM.begin(512);
  EEPROM.get(EEP_ADDR, rec);
  if (rec.magic != EEP_MAGIC) {
    rec.magic = EEP_MAGIC;
    rec.count = 0;
    rec.rsv = 0;
  }

  Serial.println();
  Serial.println(F("############################################"));
  Serial.println(F("#  SSD1680 区域局刷测试  (缩窗 + 减门线)   #"));
  Serial.println(F("############################################"));
  Serial.printf("BUSY idle = %d (0=空闲正常)\r\n", digitalRead(PIN_BUSY));
  dumpRec();

  // ★ 全自动：开机依次跑 296(基线) -> 56 -> 100 -> 160，不需要按键
  Serial.println(F("\r\n自动开跑：296(基线) -> 56 -> 100 -> 160"));
  delay(1500);
  for (uint8_t i = 0; i < planLen; i++) runStep();

  Serial.println(F("\r\n================ 全部完成 ================"));
  dumpRec();
  Serial.println(F("之后按右键可循环重跑"));
}

void loop()
{
  static uint8_t last = HIGH;
  uint8_t now = digitalRead(PIN_KEY);

  if (last == HIGH && now == LOW) {
    delay(20);                              // 消抖
    if (digitalRead(PIN_KEY) == LOW) {
      runStep();
      while (digitalRead(PIN_KEY) == LOW) { delay(10); yield(); }
    }
  }
  last = digitalRead(PIN_KEY);
  delay(5);
}
