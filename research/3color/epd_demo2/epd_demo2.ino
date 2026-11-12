/*
 * 演示 v2 —— 横屏 + 红黑分区 + 按键扫描
 *
 * 三条改进（来自实测反馈）：
 *   1. 横屏：setRotation(1)，296x128
 *   2. 红黑分区：频繁变化的用黑色(100℃ 区域刷)，不常变的用红色(65℃ 整页刷)
 *      - 时间区 y=0..72 纯黑，每次跳变只刷这块 → 1.9s
 *      - 温度/湿度/名言 y=72..128 红色，只在切页时 65℃ 全刷 → 红能保住
 *      （区域刷会漂白「被扫描区域内的红」，所以时间区必须纯黑）
 *   3. 按键扫描：同时监听 GPIO 5/12/13/14，按下哪个立刻显示在屏幕上
 *      之前只试了 GPIO5 没反应，说明引脚不是我猜的那个
 */

#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W  296       // 横屏宽
#define H  128       // 横屏高

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

// 候选按键脚（都不与屏幕冲突）
static const uint8_t KEYPINS[] = { 5, 12, 13, 14 };
#define NKEY 4
static int8_t lastKey = -1;      // 最后按下的候选脚下标

// ============ 页面状态 ============
static uint8_t page     = 0;     // 0=时钟 1=菜单 2=阅读
static uint8_t clockMin = 34;
static uint8_t menuSel  = 0;
static uint8_t readPage = 1;
static uint32_t lastMs  = 0;

// ============ 时钟页（横屏） ============
// 时间区 y=0..72 纯黑；y=72 以下可含红
static void drawClock(bool withRed, uint16_t h)
{
  display.fillScreen(GxEPD_WHITE);

  // 顶部细状态栏
  display.fillRect(0, 0, W, 16, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 5);
  display.print("CLOCK");
  display.setCursor(60, 5);
  display.print("3.9V");
  display.setCursor(120, 5);
  display.print("WED");
  // 右上角显示按键扫描结果，方便定位引脚
  display.setCursor(200, 5);
  if (lastKey < 0) display.print("KEY: none");
  else             display.printf("KEY: GPIO%u", KEYPINS[lastKey]);

  // 时间：黑，大字
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(4);
  display.setCursor(8, 24);
  display.printf("%02u:%02u", 12, clockMin);

  display.setTextSize(2);
  display.setCursor(170, 30);
  display.printf(":%02u", 8);

  // 本步耗时（黑，属于时间区，每次都会更新）
  display.setTextSize(1);
  display.setCursor(8, 62);
  display.printf("%lu ms", (unsigned long)lastMs);

  if (h <= 72) return;    // 区域刷到此为止，下面属于「不扫」区

  // ---- 以下只在整页刷新（65℃）时画，含红色 ----
  display.drawFastHLine(0, 74, W, GxEPD_BLACK);

  display.setTextColor(GxEPD_RED);
  display.setTextSize(2);
  display.setCursor(8, 82);
  display.print("2026-11-11");

  display.setTextSize(1);
  display.setCursor(8, 106);
  display.print("SUNNY 26C   HUM 65%");

  display.setTextColor(GxEPD_BLACK);
  display.setCursor(150, 106);
  display.print("\"Stay hungry, stay foolish.\"");

  display.setTextSize(1);
  display.setCursor(8, 118);
  display.print("POWERED BY ESP8266 + SSD1680 3C");
}

// ============ 菜单页（横屏） ============
static const char *MENU[] = { "Reading", "Weather", "Clock", "Settings", "About" };
#define NMENU 5

static void drawMenu(uint16_t h)
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 20, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 7);
  display.print("MENU");
  display.setCursor(200, 7);
  if (lastKey < 0) display.print("KEY: none");
  else             display.printf("KEY: GPIO%u", KEYPINS[lastKey]);

  for (uint8_t i = 0; i < NMENU; i++) {
    int16_t y = 24 + i * 20;
    if (y + 18 > (int16_t)h && h < H) break;
    if (y + 18 > H) break;
    bool sel = (i == menuSel);
    if (sel) {
      display.fillRect(0, y, W, 18, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
    } else {
      display.setTextColor(GxEPD_BLACK);
    }
    display.setTextSize(2);
    display.setCursor(12, y + 1);
    display.print(MENU[i]);
    if (sel) {
      display.setTextSize(1);
      display.setCursor(240, y + 5);
      display.print(">");
    }
  }
}

// ============ 阅读页（横屏） ============
static const char *PARA[] = {
  "The wind was rising over the harbour and the boats",
  "began to pull against their lines, one after another,",
  "as if the whole fleet had decided to leave before dawn.",
  "Nobody on the pier said a word. They only watched the",
  "masts swing, and the lamps rock, and the water break",
  "into small white teeth along the edge of the stone.",
};
#define NPARA 6

static void drawReading(uint16_t h)
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 18, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 5);
  display.printf("PAGE %u / 12", readPage);
  display.setCursor(200, 5);
  if (lastKey < 0) display.print("KEY: none");
  else             display.printf("KEY: GPIO%u", KEYPINS[lastKey]);

  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  for (uint8_t i = 0; i < NPARA; i++) {
    int16_t y = 24 + i * 16;
    if (y + 12 > (int16_t)h && h < H) break;
    if (y + 12 > H) break;
    display.setCursor(6, y);
    display.print(PARA[i]);
  }
}

// ============ 刷新 ============
static uint32_t doPage(uint16_t h, uint8_t temp, uint8_t mode, bool withRed)
{
  display.epd2.setFastTemp(temp);
  display.epd2.setUpdateMode(mode);
  // ★ 横屏下 GDR 不能用：横屏的一行文字物理上横跨全部 296 条门线，
  //   减门线会让扫描区和高亮区错位。这里统一走全门线，靠 RAM 窗口限制写入。
  display.epd2.setGateLines(0);

  uint32_t t0 = millis();
  if (h >= H) display.setFullWindow();
  else        display.setPartialWindow(0, 0, W, h);

  display.firstPage();
  do {
    if (page == 0)      drawClock(withRed, h);
    else if (page == 1) drawMenu(h);
    else                drawReading(h);
  } while (display.nextPage());

  return millis() - t0;
}

// ============ 步骤机 ============
static uint8_t step = 0;

static void runStep()
{
  uint32_t t;
  switch (step) {
    case 0:
      page = 0; lastMs = 0;
      Serial.println(F("[0] 时钟页 整页 65C (含红)"));
      lastMs = doPage(H, 65, 0xD7, true);
      break;
    case 1: case 2: case 3: case 4:
      clockMin = (clockMin + 1) % 60;
      Serial.printf("[%u] 时间跳变 区域72 100C\r\n", step);
      lastMs = doPage(72, 100, 0xD7, false);
      break;
    case 5:
      page = 1; menuSel = 0; lastMs = 0;
      Serial.println(F("[5] 切菜单 整页 100C"));
      lastMs = doPage(H, 100, 0xD7, false);
      break;
    case 6: case 7: case 8:
      menuSel = (menuSel + 1) % NMENU;
      Serial.printf("[%u] 高亮下移 区域128 100C\r\n", step);
      lastMs = doPage(H, 100, 0xD7, false);
      break;
    case 9:
      page = 2; readPage = 1; lastMs = 0;
      Serial.println(F("[9] 切阅读 整页 100C"));
      lastMs = doPage(H, 100, 0xD7, false);
      break;
    case 10: case 11:
      readPage++;
      Serial.printf("[%u] 阅读翻页 整页 100C\r\n", step);
      lastMs = doPage(H, 100, 0xD7, false);
      break;
    case 12:
      page = 0; clockMin = 0; lastMs = 0;
      Serial.println(F("[12] 回时钟 整页 65C (清残影)"));
      lastMs = doPage(H, 65, 0xD7, true);
      break;
  }
  Serial.printf("    -> %lu ms\r\n", (unsigned long)lastMs);
  step = (step + 1) % 13;
}

void setup()
{
  Serial.begin(115200);
  delay(300);

  // 候选按键脚全部上拉输入
  for (uint8_t i = 0; i < NKEY; i++) pinMode(KEYPINS[i], INPUT_PULLUP);

  display.init(115200, true, 20, false);
  display.setRotation(1);          // ★ 横屏

  Serial.println(F("==== 演示 v2：横屏 + 红黑分区 + 按键扫描 ===="));
  Serial.print(F("扫描引脚:"));
  for (uint8_t i = 0; i < NKEY; i++) { Serial.print(' '); Serial.print(KEYPINS[i]); }
  Serial.println();

  runStep();
}

void loop()
{
  static uint32_t tLast = 0;
  static bool inited = false;
  static uint8_t stable[NKEY] = { 1, 1, 1, 1 };

  if (!inited) { tLast = millis(); inited = true; }

  // 高频扫描：任何候选脚被拉低超过 2 次采样就认为按下
  for (uint8_t i = 0; i < NKEY; i++) {
    uint8_t v = (digitalRead(KEYPINS[i]) == LOW) ? 0 : 1;
    if (v == 0 && stable[i] == 1) {
      delay(15);
      if (digitalRead(KEYPINS[i]) == LOW) {
        lastKey = i;
        Serial.printf(">>> 检测到按键 GPIO%u\r\n", KEYPINS[i]);
        runStep();
        tLast = millis();
      }
    }
    stable[i] = v;
  }

  // 6 秒无操作自动走一步
  if (millis() - tLast > 6000) {
    runStep();
    tLast = millis();
  }
  delay(8);
}
