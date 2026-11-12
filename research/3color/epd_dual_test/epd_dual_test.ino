/*
 * 双驱动切换验证（自包含，不依赖任何第三方项目代码）
 *
 * 目标：验证同一块 SSD1680 三色屏上，能否在
 *       GxEPD2_3C<GxEPD2_290_C90c>（三色）
 *   与  GxEPD2_BW<GxEPD2_290_T5>   （黑白，SSD1680）
 * 之间来回切换，并测量：
 *   ① 三色全刷耗时
 *   ② 切到黑白后全刷耗时 + 红色残留情况
 *   ③ 再切回三色的耗时 + 能否恢复
 *   ④ 两个驱动对象共存时的可用堆大小
 *
 * 串口 115200
 */
#define ENABLE_GxEPD2_GFX 1

#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4

// 三色：SSD1680 彩色
GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  epd3c(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

// 黑白：SSD1680（GDEW029T5），与三色屏同一颗控制器
GxEPD2_BW<GxEPD2_290_T5, GxEPD2_290_T5::HEIGHT>
  epdbw(GxEPD2_290_T5(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define MY_RED 0xF800

void drawScene(GxEPD2_GFX& d, const char* title, bool withRed)
{
  d.fillScreen(GxEPD_WHITE);
  d.drawRect(0, 0, d.width(), d.height(), GxEPD_BLACK);
  d.drawRect(2, 2, d.width() - 4, d.height() - 4, GxEPD_BLACK);

  d.setTextColor(GxEPD_BLACK);
  d.setCursor(8, 24);
  d.print(title);
  d.setCursor(8, 42);
  d.print("SSD1680 128x296");

  // 上半部：纯黑块（三色/黑白都应该出来）
  d.fillRect(10, 60, 50, 40, GxEPD_BLACK);
  d.setCursor(66, 86);
  d.print("BLK");

  // 下半部：红块（黑白驱动下会怎样？这是观察重点）
  if (withRed)
    d.fillRect(10, 115, 50, 40, MY_RED);
  else
    d.drawRect(10, 115, 50, 40, GxEPD_BLACK);
  d.setCursor(66, 141);
  d.print(withRed ? "RED" : "no-red");

  // 细线测试：看黑白驱动下 1px 线是否清晰
  for (int y = 175; y < 235; y += 6)
    d.drawFastHLine(8, y, 112, GxEPD_BLACK);

  d.setCursor(8, 250);
  d.print("1px lines");
  d.setCursor(8, 268);
  d.print("ghost check");
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("===== DUAL DRIVER SWITCH TEST =====");
  Serial.printf("free heap at start: %u\n", ESP.getFreeHeap());

  // ---------- 阶段 1：三色 ----------
  Serial.println("[1] 3C init + full refresh ...");
  uint32_t t = millis();
  epd3c.init(115200, true, 2, false);
  epd3c.setFullWindow();
  epd3c.firstPage();
  do { drawScene(epd3c, "3C MODE", true); } while (epd3c.nextPage());
  uint32_t dt3c = millis() - t;
  Serial.printf("[1] 3C full = %lu ms, heap=%u\n", dt3c, ESP.getFreeHeap());
  epd3c.powerOff();
  delay(800);

  // ---------- 阶段 2：切黑白 ----------
  Serial.println("[2] switch to BW, full refresh ...");
  t = millis();
  epdbw.init(115200, true, 2, false);
  epdbw.setFullWindow();
  epdbw.firstPage();
  do { drawScene(epdbw, "BW MODE", false); } while (epdbw.nextPage());
  uint32_t dtbw = millis() - t;
  Serial.printf("[2] BW full = %lu ms, heap=%u\n", dtbw, ESP.getFreeHeap());
  epdbw.powerOff();
  delay(800);

  // ---------- 阶段 3：切回三色 ----------
  Serial.println("[3] switch back to 3C ...");
  t = millis();
  epd3c.init(115200, true, 2, false);
  epd3c.setFullWindow();
  epd3c.firstPage();
  do { drawScene(epd3c, "3C AGAIN", true); } while (epd3c.nextPage());
  uint32_t dt3c2 = millis() - t;
  Serial.printf("[3] 3C full again = %lu ms, heap=%u\n", dt3c2, ESP.getFreeHeap());

  Serial.println("========== SUMMARY ==========");
  Serial.printf("3C  first : %lu ms\n", dt3c);
  Serial.printf("BW  switch: %lu ms\n", dtbw);
  Serial.printf("3C  back  : %lu ms\n", dt3c2);
  Serial.printf("free heap : %u\n", ESP.getFreeHeap());
  Serial.println("=============================");
  Serial.println("看屏幕：红块是否残留？细线是否清晰？");
}

void loop()
{
  delay(10000);
}
