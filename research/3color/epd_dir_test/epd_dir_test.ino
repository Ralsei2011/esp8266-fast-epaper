/*
 * 方向实验：刷新耗时到底由「面积」决定，还是由「门线覆盖范围」决定？
 *
 * 四个窗口，面积从 4.4% 到 50% 不等，逐一计时。
 * 100℃ 快波形，每次测试前先整屏刷白保证干净。
 *
 * 预期结论：
 *   耗时只跟「窗口在门线方向上覆盖多少条门线」有关，
 *   跟面积、跟高度完全无关。
 */

#define ENABLE_GxEPD2_GFX 1
#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W  296
#define H  128

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

static void pinsOk()
{
  pinMode(PIN_CS, OUTPUT); pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_BUSY, INPUT); pinMode(PIN_DC, OUTPUT);
}

// 整屏刷白 + 画外框，作为每次测试的干净底
static void cleanWhite()
{
  pinsOk();
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(0, 0, W, H, GxEPD_BLACK);
  } while (display.nextPage());
}

static void runCase(const char *name, int16_t x, int16_t y,
                    int16_t w, int16_t h, uint16_t gdr)
{
  cleanWhite();

  pinsOk();
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(gdr);          // ★ 有效门线数

  display.setPartialWindow(x, y, w, h);

  uint32_t t0 = millis();
  display.firstPage();
  do {
    display.fillRect(x, y, w, h, GxEPD_BLACK);
  } while (display.nextPage());
  uint32_t dt = millis() - t0;

  char area[16];
  sprintf(area, "%.1f%%", 100.0f * w * h / (float)(W * H));
  Serial.printf("%-22s 窗口 %3dx%-3d 面积 %5s 门线 %3u -> %5lu ms\r\n",
                name, w, h, area, gdr, (unsigned long)dt);
}

void setup()
{
  Serial.begin(115200);
  delay(600);

  display.init(115200, true, 20, false);
  display.setRotation(3);      // IC 边朝下，快刷带贴右

  Serial.println("");
  Serial.println(F("===== 方向实验：面积 vs 门线 ====="));
  Serial.println(F("窗口坐标按 rotation 3 逻辑坐标，快刷带在右侧"));

  // A：横贯整屏的细横带 —— 面积仅 4.4%，但横跨全部 296 条门线
  runCase("A 细横带 296x16", 0, 0, 296, 16, 296);

  // B：右侧竖带 144 宽 —— 面积 48.6%
  runCase("B 右竖带 144x128", 152, 0, 144, 128, 144);

  // C：右侧细竖带 72 宽 —— 面积 24.3%
  runCase("C 右竖带 72x128", 224, 0, 72, 128, 72);

  // D：横向粗横带 296x64 —— 面积 50%，门线仍 296
  runCase("D 粗横带 296x64", 0, 0, 296, 64, 296);

  // E：左侧竖带 144 宽 —— 时间会变快，但窗口不在 0..143 门线内，画不出来
  runCase("E 左竖带 144x128", 0, 0, 144, 128, 144);

  Serial.println(F("===== 测完 ====="));
}

void loop()
{
  delay(1000);
}
