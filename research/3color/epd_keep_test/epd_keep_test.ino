/*
 * 区域刷「保留效果」观察 —— 刷完停住，让人肉眼看下方有没有被破坏
 *
 * 流程：
 *   1. 65℃ 满屏擦白（比 100℃ 彻底）
 *   2. 65℃ 满屏画参考图（含红块、黑横带、棋盘）
 *   3. 100℃ 只刷顶部 56 行
 *   4. 停住
 *
 * 要看的：
 *   - 顶部 56 行是否变成 H56 内容（黑条 + 字 + 灰红条 + 竖线）
 *   - y=60..96 的 4x4 棋盘有没有被破坏
 *   - y=100..139 的纯红块：还在吗？变灰了吗？（最关键）
 *   - y=150+ 的黑横带：位置对不对？
 *
 * 如果红块被漂白 → 区域刷会扰动未扫描区，时钟页/菜单不能用。
 * 如果红块原样保留 → 区域刷可以安全用于日常刷新。
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

// 参考图：整屏，重点在 y=100..139 放一整条纯红
static void drawBase()
{
  display.fillScreen(GxEPD_WHITE);

  // 顶部 56 行（会被后面的区域刷覆盖，先放点东西）
  display.fillRect(0, 0, W, 20, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  display.setCursor(2, 3);
  display.print("BEFORE");

  // y=60..96  4x4 棋盘 —— 区域刷 h=56 不该碰到它
  for (int16_t y = 0; y < 36; y += 4)
    for (int16_t x = 0; x < W; x += 4)
      if (((x / 4) + (y / 4)) & 1)
        display.fillRect(x, 60 + y, 4, 4, GxEPD_BLACK);

  // ★ y=100..139 纯红块 —— 最关键观察点，绝对不该变
  display.fillRect(0, 100, W, 40, GxEPD_RED);

  // y=150+ 每 20 行一条黑横带
  for (int16_t y = 150; y < H; y += 20)
    display.fillRect(0, y, W, 10, GxEPD_BLACK);

  // 底部再放一行字，确认最远端也没被动
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  display.setCursor(2, H - 24);
  display.print("BOTTOM");
}

// 只画顶部 56 行的新内容
static void drawTop56()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 26, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  display.setCursor(2, 4);
  display.print("H56 AFTER");
  // y=28..38 一条红：100℃ 下应该变灰，正好和下方那条「真红」形成对照
  display.fillRect(0, 28, W, 10, GxEPD_RED);
  for (int16_t x = 0; x < W; x += 3)
    display.drawFastVLine(x, 42, 12, GxEPD_BLACK);
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  EEPROM.begin(512);

  display.init(115200, true, 20, false);

  // 1) 65℃ 满屏擦白
  Serial.println(F("[1] 65C 擦白 ..."));
  display.epd2.setFastTemp(65);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(0);
  uint32_t t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
  Serial.printf("    -> %lu ms\r\n", (unsigned long)(millis() - t0));

  // 2) 65℃ 画参考图（红色正确）
  Serial.println(F("[2] 65C 参考图 ..."));
  t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do { drawBase(); } while (display.nextPage());
  Serial.printf("    -> %lu ms\r\n", (unsigned long)(millis() - t0));

  delay(2000);

  // 3) 100℃ 只刷顶部 56 行
  Serial.println(F("[3] 100C 区域刷 h=56 ..."));
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(56);
  t0 = millis();
  display.setPartialWindow(0, 0, W, 56);
  display.firstPage();
  do { drawTop56(); } while (display.nextPage());
  uint32_t t = millis() - t0;
  Serial.printf("    -> %lu ms\r\n", (unsigned long)t);

  Serial.println(F("======== 停住，看屏幕 ========"));
  Serial.println(F("顶部 H56 AFTER 是否出现？下方红块还是红的吗？"));

  display.hibernate();
}

void loop() { delay(1000); }
