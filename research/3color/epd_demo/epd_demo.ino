/*
 * 迷你体验演示 —— 验证「快波形 + 区域刷」的真实手感
 *
 * 三个页面，覆盖三种刷新场景：
 *   时钟页：时间(黑) 用 100℃ 区域刷 1.7s，日期(红) 只在切页时用 65℃ 全刷
 *   菜单页：100℃ 满屏切页 5.3s，高亮移动用 100℃ 区域刷 3.1s
 *   阅读页：100℃ 满屏切页 5.3s，翻页用 100℃ 区域刷 3.1s
 *
 * 交互：每 4 秒自动走一步；按右键(GPIO5)立即走一步。
 *   注意中间的键是 GPIO0（烧录模式脚），按它会复位，别按。
 *
 * 屏幕右上角会显示每步的耗时，直接能看到数字。
 */

#include <GxEPD2_3C.h>
#include <EEPROM.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4
#define PIN_KEY   5      // 右键，低电平按下

#define W  128
#define H  296

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

// ============ 页面状态 ============
static uint8_t  page      = 0;   // 0=时钟 1=菜单 2=阅读
static uint8_t  clockMin  = 34;
static uint8_t  menuSel   = 0;
static uint8_t  readPage  = 1;
static uint32_t lastMs    = 0;   // 上一步耗时，画在屏幕上

// ============ 时钟页 ============
// withRed=true 时画红色日期（必须配 65℃ 全刷）；
// withRed=false 只画黑白（可配 100℃ 区域刷，红色区域留空不碰）
static void drawClock(bool withRed, uint16_t h)
{
  display.fillScreen(GxEPD_WHITE);

  // 顶部状态栏（黑底白字）
  display.fillRect(0, 0, W, 14, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(1);
  display.setCursor(2, 4);
  display.print("CLOCK");
  display.setCursor(96, 4);
  display.print("3.9V");

  // 时间：黑，textSize 3
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(3);
  display.setCursor(6, 22);
  display.printf("%02u:%02u", 12, clockMin);

  // 秒/步耗时：黑，textSize 1
  display.setTextSize(1);
  display.setCursor(2, 50);
  display.printf("%lu ms  TEMP100", (unsigned long)lastMs);

  if (h <= 60) return;   // 区域刷只覆盖时间区，以下内容属于「不扫」的部分

  // 日期：红色，textSize 2  ← 关键：这一行是红的
  if (withRed) {
    display.setTextColor(GxEPD_RED);
    display.setTextSize(2);
    display.setCursor(4, 68);
    display.print("2026-11-11");
    display.setCursor(4, 90);
    display.print("WED  Good!");
  } else {
    // 纯黑白版本（区域刷场景下当作它不存在）
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(4, 68);
    display.print("2026-11-11");
  }

  // 下方的天气块（黑白），证明远端的字没被扰动
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  display.setCursor(4, 120);
  display.print("SHENZHEN");
  display.setTextSize(1);
  display.setCursor(4, 146);
  display.print("SUNNY  26C  HUM 65%");
  for (int16_t y = 166; y < H; y += 20)
    display.fillRect(0, y, W, 8, GxEPD_BLACK);
}

// ============ 菜单页 ============
static const char *MENU[] = { "Reading", "Weather", "Clock", "Settings", "About" };
#define NMENU 5

static void drawMenu(uint16_t h)
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 30, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  display.setCursor(4, 6);
  display.print("MENU");

  for (uint8_t i = 0; i < NMENU; i++) {
    int16_t y = 40 + i * 30;
    if (y + 26 > (int16_t)h) break;         // 超出本次扫描范围就不画
    bool sel = (i == menuSel);
    if (sel) {
      display.fillRect(0, y, W, 26, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
    } else {
      display.setTextColor(GxEPD_BLACK);
    }
    display.setTextSize(2);
    display.setCursor(10, y + 5);
    display.print(MENU[i]);
  }

  if ((int16_t)h >= 200) {
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);
    display.setCursor(2, 200);
    display.print("UP/DOWN to move");
  }
}

// ============ 阅读页 ============
static const char *PARA[] = {
  "The wind was rising",
  "over the harbour and",
  "the boats began to",
  "pull against their",
  "lines, one after",
  "another, as if the",
  "whole fleet had",
  "decided to leave",
  "before dawn.",
};
#define NPARA 9

static void drawReading(uint16_t h)
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 26, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  display.setCursor(4, 4);
  display.printf("PAGE %u", readPage);

  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  for (uint8_t i = 0; i < NPARA; i++) {
    int16_t y = 36 + i * 26;
    if (y + 22 > (int16_t)h) break;
    display.setCursor(4, y);
    display.print(PARA[(i + readPage) % NPARA]);
  }

  if ((int16_t)h >= 280) {
    display.setTextSize(1);
    display.setCursor(2, 282);
    display.printf("-- %lu ms --", (unsigned long)lastMs);
  }
}

// ============ 刷一整页（含内容绘制） ============
static uint32_t doPage(uint16_t h, uint8_t temp, uint8_t mode, bool withRed)
{
  display.epd2.setFastTemp(temp);
  display.epd2.setUpdateMode(mode);
  display.epd2.setGateLines(h);

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
    // ---- 进入时钟页：整页 65℃，因为有红色日期 ----
    case 0:
      page = 0; lastMs = 0;
      Serial.println(F("[0] 时钟页 整页 65C (含红日期)"));
      t = doPage(H, 65, 0xD7, true);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;

    // ---- 时间跳变：只刷顶部 60 行，100℃ ----
    case 1: case 2: case 3:
      clockMin = (clockMin + 1) % 60;
      Serial.printf("[%u] 时间跳变 区域60 100C\r\n", step);
      t = doPage(60, 100, 0xD7, false);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;

    // ---- 切菜单：整页 100℃ ----
    case 4:
      page = 1; menuSel = 0; lastMs = 0;
      Serial.println(F("[4] 切菜单页 整页 100C"));
      t = doPage(H, 100, 0xD7, false);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;

    // ---- 高亮移动：刷 148 行，100℃ ----
    case 5: case 6: case 7:
      menuSel = (menuSel + 1) % NMENU;
      Serial.printf("[%u] 高亮下移 区域148 100C\r\n", step);
      t = doPage(148, 100, 0xD7, false);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;

    // ---- 切阅读页：整页 100℃ ----
    case 8:
      page = 2; readPage = 1; lastMs = 0;
      Serial.println(F("[8] 切阅读页 整页 100C"));
      t = doPage(H, 100, 0xD7, false);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;

    // ---- 翻页：刷 148 行 ----
    case 9: case 10:
      readPage++;
      Serial.printf("[%u] 阅读翻页 区域148 100C\r\n", step);
      t = doPage(148, 100, 0xD7, false);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;

    // ---- 收尾：回到时钟页，顺便清残影（65℃ 整页） ----
    case 11:
      page = 0; clockMin = 0; lastMs = 0;
      Serial.println(F("[11] 回时钟页 整页 65C (清残影)"));
      t = doPage(H, 65, 0xD7, true);
      lastMs = t;
      Serial.printf("    -> %lu ms\r\n", (unsigned long)t);
      break;
  }

  step = (step + 1) % 12;
  Serial.printf("    step %u done, 按右键或等 4 秒\r\n", step);
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_KEY, INPUT_PULLUP);

  display.init(115200, true, 20, false);
  Serial.println(F("==== 迷你演示开始 ===="));
  Serial.println(F("右键=GPIO5 走一步；中间键=GPIO0 会复位，别按"));
  runStep();
}

void loop()
{
  static uint32_t tLast = 0;
  static bool wasDown = false;
  static bool inited = false;
  if (!inited) { tLast = millis(); inited = true; }

  // 按键：低电平按下，带 30ms 去抖
  bool down = (digitalRead(PIN_KEY) == LOW);
  if (down && !wasDown) {
    delay(30);
    if (digitalRead(PIN_KEY) == LOW) {
      runStep();
      tLast = millis();
    }
  }
  wasDown = down;

  // 4 秒无操作自动走一步
  if (millis() - tLast > 4000) {
    runStep();
    tLast = millis();
  }
  delay(10);
}
