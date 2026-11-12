/*
 * 「区域刷 + 快波形」叠加验证
 *
 * 已知：
 *   - 基线波形（0xF7）区域刷：t ≈ 2.86s + 55.8ms × 行数  (56行=5.98s, 296行=19.4s)
 *   - 100℃ 快波形（0xD7）满屏：5.29s
 *   - 100℃ 会把红色刷成灰白 → 纯黑白页面专用
 *
 * 要回答：两者的加速能不能叠加？100℃ 下刷 56 行到底几秒？
 *
 * 顺带观察：
 *   - 区域刷不覆盖的下方红块会不会被 100℃ 波形漂成灰
 *   - 区域外的内容会不会被破坏
 *
 * 流程（一次跑完，约 35 秒）：
 *   1. 100℃ 满屏擦白
 *   2. 65℃ 满屏画底图（含红块，作为参考）
 *   3. 100℃ 区域刷 h=56 / 100 / 148
 *   4. 100℃ 满屏 296
 *
 * 每步耗时写 EEPROM，串口掉了也能在下次开机回读。
 */

#include <GxEPD2_3C.h>
#include <EEPROM.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W  128
#define H  296

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define EEP_ADDR   0
#define EEP_MAGIC  0xC600ULL
#define NSTEP      5

struct Rec {
  uint64_t magic;
  uint32_t ms[NSTEP];
  uint8_t  n;
};

// 底图：整屏画，含红块（y=100..139），供后面区域刷时观察红有没有变
static void drawBase()
{
  display.fillScreen(GxEPD_WHITE);
  // 顶部 56 行：黑横条 + 白字
  display.fillRect(0, 0, W, 20, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  display.setCursor(2, 3);
  display.print("BASE");
  // 20..56 竖线（难度参考）
  for (int16_t x = 0; x < W; x += 3)
    display.drawFastVLine(x, 24, 28, GxEPD_BLACK);
  // 60..96 4x4 棋盘
  for (int16_t y = 0; y < 36; y += 4)
    for (int16_t x = 0; x < W; x += 4)
      if (((x / 4) + (y / 4)) & 1) display.fillRect(x, 60 + y, 4, 4, GxEPD_BLACK);
  // ★ 100..139 纯红块：区域刷 h=56/100 时不覆盖它，看它会不会被漂白
  display.fillRect(0, 100, W, 40, GxEPD_RED);
  // 150..295 每 20 行一条黑横带（高行号区域，h=148/296 才会扫到）
  for (int16_t y = 150; y < H; y += 20)
    display.fillRect(0, y, W, 10, GxEPD_BLACK);
}

// 区域内容：黑条 + 大字，用来确认这一块真的刷新了
static void drawRegion(uint16_t h, const char *tag)
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 26, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  display.setCursor(2, 4);
  display.print(tag);
  // 一条红（y=28..38）：100℃ 下应明显变灰，正好当视觉锚点
  display.fillRect(0, 28, W, 10, GxEPD_RED);
  // 红条以下的竖线区，尽量填满本区域
  int16_t y0 = 42;
  int16_t hh = (int16_t)h - y0 - 2;
  if (hh > 2)
    for (int16_t x = 0; x < W; x += 3)
      display.drawFastVLine(x, y0, hh, GxEPD_BLACK);
}

static uint32_t refreshRegion(uint16_t h, uint8_t temp, uint8_t mode,
                              const char *tag, bool drawContent)
{
  display.epd2.setFastTemp(temp);
  display.epd2.setUpdateMode(mode);
  display.epd2.setGateLines(h);          // 必须在 setPartialWindow 之前

  uint32_t t0 = millis();
  if (h >= H) {
    display.setFullWindow();
    display.firstPage();
    do { if (drawContent) drawRegion(h, tag); } while (display.nextPage());
  } else {
    display.setPartialWindow(0, 0, W, h);
    display.firstPage();
    do { if (drawContent) drawRegion(h, tag); } while (display.nextPage());
  }
  return millis() - t0;
}

void setup()
{
  Serial.begin(115200);
  delay(300);

  EEPROM.begin(512);
  Rec rec;
  EEPROM.get(EEP_ADDR, rec);
  if (rec.magic == EEP_MAGIC && rec.n > 0) {
    Serial.println(F("---- 上次记录 ----"));
    for (uint8_t i = 0; i < rec.n && i < NSTEP; i++)
      Serial.printf("   step %u : %lu ms\r\n", i, (unsigned long)rec.ms[i]);
  }
  rec.magic = EEP_MAGIC;
  rec.n = 0;
  EEPROM.put(EEP_ADDR, rec);
  EEPROM.commit();

  display.init(115200, true, 20, false);

  // 1) 100℃ 满屏擦白
  Serial.println(F("[1] 100C 满屏擦白 ..."));
  uint32_t t = refreshRegion(H, 100, 0xD7, "WIPE", false);
  Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
  rec.ms[rec.n++] = t; EEPROM.put(EEP_ADDR, rec); EEPROM.commit();

  // 2) 65℃ 满屏画底图（含红）
  Serial.println(F("[2] 65C 满屏 底图 ..."));
  display.epd2.setFastTemp(65);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(0);
  uint32_t t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do { drawBase(); } while (display.nextPage());
  t = millis() - t0;
  Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
  rec.ms[rec.n++] = t; EEPROM.put(EEP_ADDR, rec); EEPROM.commit();

  delay(1500);

  // 3) 100℃ 区域刷三档
  const uint16_t hs[3] = { 56, 100, 148 };
  const char *tags[3] = { "H56", "H100", "H148" };
  for (uint8_t i = 0; i < 3; i++) {
    Serial.printf("[3.%u] 100C 区域 h=%u ...\r\n", i + 1, hs[i]);
    t = refreshRegion(hs[i], 100, 0xD7, tags[i], true);
    Serial.printf("    -> %lu ms  (%.1f ms/行)\r\n",
                  (unsigned long)t, (float)t / hs[i]);
    rec.ms[rec.n++] = t; EEPROM.put(EEP_ADDR, rec); EEPROM.commit();
    delay(800);
  }

  // 4) 100℃ 满屏
  Serial.println(F("[4] 100C 满屏 ..."));
  t = refreshRegion(H, 100, 0xD7, "FULL", true);
  Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
  rec.ms[rec.n++] = t; EEPROM.put(EEP_ADDR, rec); EEPROM.commit();

  Serial.println(F("======== 全部完成 ========"));
  Serial.printf("wipe100=%lu base65=%lu h56=%lu h100=%lu h148=%lu full100=%lu\r\n",
                (unsigned long)rec.ms[0], (unsigned long)rec.ms[1],
                (unsigned long)rec.ms[2], (unsigned long)rec.ms[3],
                (unsigned long)rec.ms[4], (unsigned long)t);
  Serial.println(F("看屏幕：最后一次是 FULL，之前三档的痕迹都该被覆盖"));

  display.hibernate();
}

void loop() { delay(1000); }
