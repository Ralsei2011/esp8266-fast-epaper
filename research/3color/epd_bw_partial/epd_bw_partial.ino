/*
 * 三色屏(SSD1680) 用黑白驱动做局部刷新 —— 可行性与残影测试
 * 目的：验证 GxEPD2_290_T94(BW, 局刷 500ms) 驱动这块三色屏时的
 *       ① 全刷是否正常 ② 局刷速度 ③ 是否有红色残影/发灰
 * 串口 115200
 */
#include <SPI.h>
#include <GxEPD2_BW.h>

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4

GxEPD2_BW<GxEPD2_290_T94, GxEPD2_290_T94::HEIGHT>
  display(GxEPD2_290_T94(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

int cnt = 0;
int boxY = 0;

void drawFrame()
{
  display.fillScreen(GxEPD_WHITE);
  display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
  // 顶部：标题区（局刷会覆盖这里）
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(6, 18);
  display.print("BW PARTIAL TEST");
  display.setCursor(6, 34);
  display.print("SSD1680 / T94 LUT");

  // 中部：红块位置标记 —— 如果这里原来有红色，全刷后应变成白
  display.drawRect(20, 120, 88, 40, GxEPD_BLACK);
  display.setCursor(24, 146);
  display.print("RED AREA?");

  // 底部：说明
  display.setCursor(6, 250);
  display.print("watch ghosting here");
  display.setCursor(6, 268);
  display.print("partial every 3s");
}

void fullRefresh()
{
  uint32_t t = millis();
  display.setFullWindow();
  display.firstPage();
  do { drawFrame(); } while (display.nextPage());
  Serial.printf("[FULL] %lu ms\n", millis() - t);
}

void partialRefresh()
{
  uint32_t t = millis();
  // 只在顶部 0..56 像素高的横条里更新，模拟菜单/时钟的局部刷新
  display.setPartialWindow(0, 0, display.width(), 56);
  display.firstPage();
  do {
    display.fillRect(0, 0, display.width(), 56, GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(6, 18);
    display.print("PARTIAL #");
    display.print(cnt);
    display.setCursor(6, 34);
    display.print("heap ");
    display.print(ESP.getFreeHeap());
    // 一个移动的小方块，用来观察残影
    display.fillRect(boxY, 40, 16, 12, GxEPD_BLACK);
  } while (display.nextPage());
  Serial.printf("[PART %d] %lu ms\n", cnt, millis() - t);
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("===== BW PARTIAL TEST on 3-color panel =====");
  display.init(115200, true, 2, false);
  Serial.printf("panel %dx%d  hasFastPartial=%d\n", display.width(), display.height(),
                display.epd2.hasFastPartialUpdate);
  fullRefresh();
  Serial.println("全刷完成，接下来每 3 秒局刷一次顶部横条");
}

void loop()
{
  delay(3000);
  cnt++;
  boxY = (boxY + 24) % 112;
  partialRefresh();
  if (cnt % 10 == 0) {
    Serial.println("-- 每 10 次做一次全刷，清除累积残影 --");
    fullRefresh();
  }
}
