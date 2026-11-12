/*
 * 2.9" 电子墨水屏 驱动探针  (ESP8266 / 甘草酸不酸 2.9寸 SD 墨水屏阅读器板)
 * ---------------------------------------------------------------------
 * 手上这批 2.9" 三色屏标签是 E029A1ON1842xx，标签不写控制 IC，
 * 所以用「逐个试」的办法确认：编译时 -DDRV=n
 *
 *   DRV 1 = GxEPD2_290_C90c   SSD1680  三色   (2021 年后最常见的 2.9" 三色屏)
 *   DRV 2 = GxEPD2_290c       IL0373   三色   (老款 2.9" 三色屏)
 *   DRV 3 = GxEPD2_290_Z13c   UC8151   三色   (GDEH029Z13)
 *   DRV 4 = GxEPD2_290_T94    SSD1680  黑白
 *   DRV 5 = GxEPD2_290        IL3820   黑白   (板子原厂固件用的驱动)
 *
 * 板载 FPC 固定接线：CS=GPIO15  DC=GPIO0  RST=GPIO2  BUSY=GPIO4
 * 屏幕表现判读：左红右黑 = 命中；全白/全黑/花屏/无反应 = 换下一档
 */

#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>

#ifndef DRV
#define DRV 1
#endif

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4

#if DRV == 1
  #define DRV_NAME "GxEPD2_290_C90c / SSD1680 / 3C"
  GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
    display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define IS_3C 1
#elif DRV == 2
  #define DRV_NAME "GxEPD2_290c / IL0373 / 3C"
  GxEPD2_3C<GxEPD2_290c, GxEPD2_290c::HEIGHT>
    display(GxEPD2_290c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define IS_3C 1
#elif DRV == 3
  #define DRV_NAME "GxEPD2_290_Z13c / UC8151 / 3C"
  GxEPD2_3C<GxEPD2_290_Z13c, GxEPD2_290_Z13c::HEIGHT>
    display(GxEPD2_290_Z13c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define IS_3C 1
#elif DRV == 4
  #define DRV_NAME "GxEPD2_290_T94 / SSD1680 / BW"
  GxEPD2_BW<GxEPD2_290_T94, GxEPD2_290_T94::HEIGHT>
    display(GxEPD2_290_T94(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define IS_3C 0
#else
  #define DRV_NAME "GxEPD2_290 / IL3820 / BW"
  GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT>
    display(GxEPD2_290(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define IS_3C 0
#endif

static void drawTestPage()
{
  const int16_t W = display.width();
  const int16_t H = display.height();
  Serial.printf("[draw] %dx%d\n", W, H);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // 左边一大块：3C 用红，黑白用全黑 —— 一眼能看出颜色通道
#if IS_3C
    display.fillRect(0, 0, W / 3, H, GxEPD_RED);
#else
    display.fillRect(0, 0, W / 3, H, GxEPD_BLACK);
#endif
    // 右边一大块黑
    display.fillRect(W - W / 3, 0, W / 3, H, GxEPD_BLACK);

    // 中间白区
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(48, 30);
    display.print("2.9");
    display.setCursor(48, 60);
    display.print("EPD PROBE");
    display.setCursor(48, 100);
    display.print("DRV=");
    display.print(DRV);
    display.setCursor(48, 130);
    display.print(IS_3C ? "3COLOR" : "B/W");

#if IS_3C
    // 底部三色小色块，红通道通不通最直观
    display.fillRect(W / 3 + 6, H - 46, 24, 40, GxEPD_BLACK);
    display.fillRect(W / 3 + 36, H - 46, 24, 40, GxEPD_RED);
#endif

    display.drawRect(0, 0, W, H, GxEPD_BLACK);
  } while (display.nextPage());

  Serial.println("[draw] done");
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("===== 2.9\" EPD PROBE =====");
  Serial.print("DRV   = "); Serial.println(DRV);
  Serial.println(DRV_NAME);
  Serial.printf("BUSY  before init = %d\n", digitalRead(PIN_BUSY));

  display.init(115200, true, 2, false);

  Serial.printf("BUSY  after  init = %d\n", digitalRead(PIN_BUSY));
  Serial.printf("面板尺寸 = %d x %d\n", display.width(), display.height());

  drawTestPage();

  display.hibernate();
  Serial.println("[完成] 左红右黑=驱动正确；全白/全黑/花屏=换下一档");
}

void loop()
{
  delay(10000);
}
