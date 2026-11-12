/*
 * 横屏三色中文界面 v12
 *
 * 本版变化（按用户反馈）：
 *   1. 日期字体 wqy16 + 加粗（原来 wqy14 太细看不清）
 *   2. 「周三」原来只显示「三」→ 字库子集缺字。全字体切到 chinese1(GB2312一级常用字)
 *      并在 setup 里做缺字自检，串口打印缺哪个字
 *   3. 删掉「传感器在线/未接」状态文字
 *   4. 温湿度方块：68x62 正方形 → 70x46 长方形，压低高度
 *   5. 图标移到数字左端，整体缩小
 *   6. 温度计底部圆球改「描边圆 + 实心圆」两层叠画，更圆
 *   7. 温湿度数字改红色（红在静态区，65℃ 全刷时绘制）
 */

#define ENABLE_GxEPD2_GFX 1

#include "epd_panel.h"          // 屏幕适配层：型号表 + 驱动选择，见 epd_panel.h
#include "epd_time.h"           // 走时：编译时注入 BUILD_EPOCH，以后可换 NTP
#include <U8g2_for_Adafruit_GFX.h>
#include <Wire.h>
#include <SPI.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4
#define PIN_KEY3  3
#define PIN_BATSW 12
#define PIN_BATV  A0

#define W  296
#define H  128

#define DYN_W   144
#define DYN_X   (W - DYN_W)

EPD_DISPLAY_T display(EPD_DRIVER(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8f;

// ================= 字体 =================
// ★ 坑：chinese1/2/3 是把 GB2312 汉字按码位顺序平分成三包，不是按常用度分。
//   任选一包都会缺掉约 2/3 的字（实测 chinese1 缺「月」「日」「周」「三」…，
//   chinese3 缺「周」），缺字会静默画成空白。
//   → 必须用 gb2312 全包（6763 汉字 + 682 符号，含 ℃）。
//   全包体积大，所以只保留 12 / 16 两个字号，其余靠数字字体和加粗区分层次。
#define F_SMALL   u8g2_font_wqy12_t_gb2312
#define F_BODY    u8g2_font_wqy12_t_gb2312
#define F_TITLE   u8g2_font_wqy16_t_gb2312
#define F_WEEK    u8g2_font_wqy16_t_gb2312
#define F_DATE    u8g2_font_wqy16_t_gb2312
#define F_MENU    u8g2_font_wqy16_t_gb2312
#define F_NUM_S   u8g2_font_fub20_tn
#define F_NUM_L   u8g2_font_fub35_tn
#define F_SYM     u8g2_font_wqy16_t_gb2312

// ================= 中文 / 数字绘制 =================
// ★ 加粗等级：墨水屏上红色笔画只有 1~2px 时，红色粒子铺不满，视觉上发虚发粉。
//   2 = 横向加粗，3 = 横+竖，4 = 四向（笔画实宽 2px，红色最饱满）
#define BOLD_MODE 4

// strength 0 = 不加粗 / 1 = 四向（笔画 2x 实宽）/ 2 = 六向（四边各 +1px，小符号用）
static void drawGlyphEx(int16_t x, int16_t y, const char *s, uint8_t strength)
{
  u8f.setCursor(x, y); u8f.print(s);
  if (strength == 0) return;
  u8f.setCursor(x + 1, y); u8f.print(s);
  if (BOLD_MODE >= 3) {
    u8f.setCursor(x, y + 1); u8f.print(s);
    u8f.setCursor(x + 1, y + 1); u8f.print(s);
  }
  if (strength >= 2) {
    u8f.setCursor(x - 1, y); u8f.print(s);   // 补左 / 上，让笔画四边厚度一致
    u8f.setCursor(x, y - 1); u8f.print(s);
  }
}

static void drawGlyph(int16_t x, int16_t y, const char *s, bool bold)
{
  drawGlyphEx(x, y, s, bold ? 1 : 0);
}

static void cn(int16_t x, int16_t y, const char *s, const uint8_t *font,
               uint16_t color, bool bold)
{
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  drawGlyph(x, y, s, bold);
}

static void numCenter(int16_t cx, int16_t baseline, const char *s,
                      const uint8_t *font, uint16_t color, bool bold)
{
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  int16_t w = u8f.getUTF8Width(s);
  int16_t x = cx - w / 2;
  drawGlyph(x, baseline, s, bold);
}

// 两段文字并排居中（避免用空格凑，wqy 的空格宽度不可控）
static void twoPartCenter(int16_t cx, int16_t baseline,
                          const char *a, const char *b, int16_t gap,
                          const uint8_t *font, uint16_t color, bool bold)
{
  u8f.setFont(font);
  int16_t wa = u8f.getUTF8Width(a);
  int16_t wb = u8f.getUTF8Width(b);
  int16_t x = cx - (wa + gap + wb) / 2;
  cn(x, baseline, a, font, color, bold);
  cn(x + wa + gap, baseline, b, font, color, bold);
}

// 数字 + 小号单位（单位用不同字体，避免 fub20 缺符号）
static void numWithUnit(int16_t cx, int16_t baseline,
                        const char *num, const char *unit, int16_t gap,
                        uint16_t color)
{
  u8f.setFont(F_NUM_S);
  int16_t wn = u8f.getUTF8Width(num);
  u8f.setFont(F_SYM);
  int16_t wu = u8f.getUTF8Width(unit);
  int16_t x = cx - (wn + gap + wu) / 2;

  numCenter(x + wn / 2, baseline, num, F_NUM_S, color, false);
  // 单位基线略抬，跟数字底部对齐
  cn(x + wn + gap, baseline - 1, unit, F_SYM, color, false);
}

// 开机缺字自检：字库子集被分包过，缺字会静默画空白
static void fontCheck(const char *s, const uint8_t *font)
{
  u8f.setFont(font);
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    const uint8_t *st = p;
    if (*p < 0x80)                    p += 1;
    else if ((*p & 0xE0) == 0xC0)     p += 2;
    else if ((*p & 0xF0) == 0xE0)     p += 3;
    else                              p += 4;

    char buf[6];
    uint8_t n = (uint8_t)(p - st);
    if (n > 5) n = 5;
    memcpy(buf, st, n);
    buf[n] = 0;

    if (u8f.getUTF8Width(buf) <= 0) Serial.printf("  !! 缺字: %s\r\n", buf);
  }
}

// 右对齐（放方框右下角的单位符号用）
static void cnRight(int16_t right, int16_t baseline, const char *s,
                    const uint8_t *font, uint16_t color, bool bold)
{
  u8f.setFont(font);
  int16_t w = u8f.getUTF8Width(s);
  cn(right - w, baseline, s, font, color, bold);
}

// 右对齐 + 六向加粗：% 和 ℃ 在 16px 点阵里笔画只有 1px，
// 四向只往右下长，屏上边缘显不出来，看着就像"没加粗"。
// 这里四边各补 1px，实宽 3px，红色粒子才铺得满。
static void cnHeavy(int16_t right, int16_t baseline, const char *s,
                    const uint8_t *font, uint16_t color)
{
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  int16_t w = u8f.getUTF8Width(s);
  drawGlyphEx(right - w + 1, baseline, s, 2);   // 左侧多长 1px，右移 1 补偿
}

// 逐字绘制并加字距：点阵字体里半角数字会挤在一起，必须手动留缝
static int16_t utf8Advance(const uint8_t *p)
{
  if (*p < 0x80)                return 1;
  if ((*p & 0xE0) == 0xC0)      return 2;
  if ((*p & 0xF0) == 0xE0)      return 3;
  return 4;
}

static void cnSpaced(int16_t cx, int16_t baseline, const char *s,
                     const uint8_t *font, uint16_t color, bool bold,
                     uint8_t extra)
{
  u8f.setFont(font);
  u8f.setForegroundColor(color);

  // 第一遍：算总宽
  int16_t total = 0, n = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    const uint8_t *st = p;
    p += utf8Advance(p);
    char buf[6]; uint8_t k = (uint8_t)(p - st); if (k > 5) k = 5;
    memcpy(buf, st, k); buf[k] = 0;
    total += u8f.getUTF8Width(buf);
    n++;
  }
  if (n > 1) total += extra * (n - 1);

  // 第二遍：逐字画
  int16_t x = cx - total / 2;
  p = (const uint8_t *)s;
  while (*p) {
    const uint8_t *st = p;
    p += utf8Advance(p);
    char buf[6]; uint8_t k = (uint8_t)(p - st); if (k > 5) k = 5;
    memcpy(buf, st, k); buf[k] = 0;

    int16_t w = u8f.getUTF8Width(buf);
    drawGlyph(x, baseline, buf, bold);
    x += w + extra;
  }
}

// ================= 传感器 =================
static int16_t  tempC = 26;
static uint8_t  humiP = 65;
static uint8_t  battP = 78;
static float    battV = 3.9;
static bool     sensorOk = false;

// SHT30 单次测量（高重复性，无时钟拉伸）
static bool sht30Read(float *t, float *h)
{
  Wire.begin(13, 14);
  Wire.beginTransmission(0x44);
  Wire.write(0x24);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(20);
  if (Wire.requestFrom(0x44, 6) != 6) return false;

  uint8_t d[6];
  for (uint8_t i = 0; i < 6; i++) d[i] = Wire.read();

  uint16_t rt = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rh = ((uint16_t)d[3] << 8) | d[4];
  *t = -45.0f + 175.0f * (float)rt / 65535.0f;
  *h = 100.0f * (float)rh / 65535.0f;
  return true;
}

// 电池电压（V14 的多项式，不是线性关系）
static void readBatt()
{
  pinMode(PIN_BATSW, OUTPUT);
  digitalWrite(PIN_BATSW, HIGH);
  delay(2);

  uint32_t acc = 0;
  for (uint8_t i = 0; i < 16; i++) acc += analogRead(PIN_BATV);
  digitalWrite(PIN_BATSW, LOW);
  pinMode(PIN_BATSW, INPUT);

  float x = (acc / 16.0f) * 0.0009765625f * 5.607f;
  battV = x;

  float bfb = 497.50976f * x * x * x * x
              - 7442.07254f * x * x * x
              + 41515.70648f * x * x
              - 102249.34377f * x
              + 93770.99821f;
  if (bfb > 100) bfb = 100;
  else if (bfb < 3) bfb = 3;
  battP = (uint8_t)bfb;
}

// 读一次全部传感器，读完把 SPI 抢回来（13/14 是共用的）
static void pollSensors()
{
  float t, h;
  if (sht30Read(&t, &h)) {
    tempC = (int16_t)round(t);
    humiP = (uint8_t)round(h);
    sensorOk = true;
  } else {
    sensorOk = false;
  }
  readBatt();

  SPI.begin();     // ★ I2C 把 13/14 抢走了，这里还给屏幕
  pinsOk();

  Serial.printf("[传感器] %s  温度=%d 湿度=%u  电池=%.2fV %u%%\r\n",
                sensorOk ? "OK" : "无响应", tempC, humiP, battV, battP);
}

// ================= 按键 =================
#define LONG_MS 500
volatile uint32_t kDownMs = 0, kUpMs = 0;
volatile uint8_t  kState = 0;
volatile uint8_t  kEvents = 0;

void IRAM_ATTR isrKey3()
{
  if (digitalRead(PIN_KEY3) == LOW) { kDownMs = millis(); kState = 1; }
  else {
    kUpMs = millis();
    if (kState == 1) {
      uint32_t d = kUpMs - kDownMs;
      if (d >= LONG_MS) kEvents |= 2; else kEvents |= 1;
    }
    kState = 0;
  }
}

static uint8_t takeEvents()
{
  uint8_t e; noInterrupts(); e = kEvents; kEvents = 0; interrupts(); return e;
}

// ================= 状态 =================
enum { P_CLOCK = 0, P_MENU, P_READ, P_PANEL };
static uint8_t  page = P_CLOCK;
static uint8_t  cHour = 12, cMin = 34;
static uint8_t  menuSel = 0;
static uint8_t  readPage = 1;
static uint16_t partialCount = 0;
static uint32_t lastMs = 0;
static uint32_t autoStepMs = 0;
static uint32_t lastClockTick = 0;

#define CLOCK_FULL_MS (5UL * 60UL * 1000UL)   // 时钟页整屏刷新周期：5 分钟

static const char *MENU[] = { "阅读", "天气", "时钟", "设置", "屏幕" };
static const char *WDAY[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
#define NMENU 5

static const char *READTXT[] = {
  "风从港口那边起来了，船一条接一条地",
  "绷紧了缆绳，像是整支船队都打定主意",
  "要在天亮前离开。码头上没人说话，他",
  "们只是看着桅杆摇晃，看着灯影晃荡，",
  "看着海水在石岸边上碎成一小排白牙。",
  "海鸥已经飞到内陆去了。身后不知哪里",
  "有一扇门被风摔上，响了两下，然后港",
  "口又安静下来，只剩下索具的轻响。",
};
#define NLINES 8

// ================= 图标（整体缩小） =================
// 水滴：total h ≈ 15px
static void iconDrop(int16_t cx, int16_t cy, uint16_t color)
{
  display.fillTriangle(cx, cy - 8, cx - 5, cy + 1, cx + 5, cy + 1, color);
  display.fillCircle(cx, cy + 2, 5, color);
}

// 温度计：total h ≈ 24px，底部圆用「描边 + 实心」两层叠画，半径 5 时轮廓更规整
static void iconThermo(int16_t cx, int16_t cy, uint16_t color)
{
  display.drawRect(cx - 3, cy - 12, 6, 15, color);   // 管身
  display.fillRect(cx - 1, cy - 5, 2, 9, color);     // 水银柱
  display.fillRect(cx - 2, cy - 1, 4, 4, color);     // 管与球的接口，填缝
  display.drawCircle(cx, cy + 6, 5, color);          // 球轮廓
  display.fillCircle(cx, cy + 6, 3, color);          // 球心
}

// ================= 时钟页 =================
static void drawClockStatic()
{
  display.fillRect(0, 0, DYN_X, H, GxEPD_WHITE);

  // ---- 顶部：日期（wqy16 + 加粗 + 2px 字距，红色） / 周几 ----
  ClockTime ct = clockRead();
  char ds[24];
  sprintf(ds, "%d年%u月%u日", ct.year, ct.mon, ct.day);
  cnSpaced(DYN_X / 2, 17, ds, F_DATE, C_RED, true, 2);
  numCenter(DYN_X / 2, 38, WDAY[ct.wday], F_WEEK, C_RED, true);

  // ---- 两个长方形方块：湿度(左) / 温度(右) ----
  // 70x46，左右边距各 4，中间间隔 4  ->  4+70+4+70 = 148, 右余 4
  const int16_t by = 50, bw = 70, bh = 46;
  const int16_t x1 = 4, x2 = 78;
  const int16_t midY = by + bh / 2;      // 垂直中心
  const int16_t numBase = by + 33;       // fub20 数字基线
  const int16_t iconY = midY;            // 图标中心
  const int16_t unitBase = by + bh - 6;  // 单位符号基线（贴右下角）

  // 湿度
  display.drawRect(x1, by, bw, bh, GxEPD_BLACK);
  display.drawRect(x1 + 1, by + 1, bw - 2, bh - 2, GxEPD_BLACK);
  iconDrop(x1 + 11, iconY, GxEPD_BLACK);
  {
    char b[8]; sprintf(b, "%u", humiP);
    numCenter(x1 + 33, numBase, b, F_NUM_S, C_RED, true);
    // % 在 wqy16 里只有 8x16 像素，加粗必糊；保持单次绘制，右对齐贴右下角
    cnRight(x1 + bw - 5, unitBase, "%", F_SYM, C_RED, false);
  }

  // 温度
  display.drawRect(x2, by, bw, bh, GxEPD_BLACK);
  display.drawRect(x2 + 1, by + 1, bw - 2, bh - 2, GxEPD_BLACK);
  iconThermo(x2 + 11, iconY, GxEPD_BLACK);
  {
    char b[8]; sprintf(b, "%d", tempC);
    numCenter(x2 + 33, numBase, b, F_NUM_S, C_RED, true);
    cnRight(x2 + bw - 5, unitBase, "℃", F_SYM, C_RED, false);
  }

  display.drawFastVLine(DYN_X - 1, 0, H, GxEPD_BLACK);
}

static void drawClockDyn()
{
  display.fillRect(DYN_X, 0, DYN_W, H, GxEPD_WHITE);

  ClockTime ct = clockRead();
  char b[10];
  sprintf(b, "%02u:%02u", ct.hour, ct.min);
  numCenter(DYN_X + DYN_W / 2, 42, b, F_NUM_L, GxEPD_BLACK, false);

  const int16_t barX = DYN_X + 24, barW = 96;
  display.drawRect(barX, 52, barW, 10, GxEPD_BLACK);
  int16_t fillW = (int16_t)((barW - 4) * battP / 100);
  if (fillW > 0) display.fillRect(barX + 2, 54, fillW, 6, GxEPD_BLACK);

  char pb[8], vb[10];
  sprintf(pb, "%u%%", battP);
  sprintf(vb, "%.1fV", battV);
  twoPartCenter(DYN_X + DYN_W / 2, 78, pb, vb, 10, F_BODY, GxEPD_BLACK, false);

  display.drawFastHLine(DYN_X + 14, 86, DYN_W - 28, GxEPD_BLACK);
  cn(DYN_X + 22, 104, "名言占位", F_SMALL, GxEPD_BLACK, false);
  cn(DYN_X + 22, 118, "节日占位", F_SMALL, GxEPD_BLACK, false);
}

// ================= 其它页 =================
static void drawMenu()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 20, GxEPD_BLACK);
  cn(6, 16, "菜单  短按切换 / 长按进入", F_BODY, GxEPD_WHITE, true);

  for (uint8_t i = 0; i < NMENU; i++) {
    int16_t y = 24 + i * 20;
    bool sel = (i == menuSel);
    if (sel) {
      display.fillRect(0, y, W, 19, GxEPD_BLACK);
      cn(16, y + 16, MENU[i], F_MENU, GxEPD_WHITE, true);
      cn(W - 52, y + 16, "进入", F_SMALL, GxEPD_WHITE, false);
    } else {
      cn(16, y + 16, MENU[i], F_MENU, GxEPD_BLACK, true);
    }
  }
}

static void drawRead()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 18, GxEPD_BLACK);

  char t[40];
  sprintf(t, "第 %u 页 / 12   短按翻页  长按菜单", readPage);
  cn(6, 14, t, F_SMALL, GxEPD_WHITE, false);

  for (uint8_t i = 0; i < NLINES; i++) {
    cn(6, 33 + i * 13, READTXT[i], F_SMALL, GxEPD_BLACK, false);
  }
}

// ================= 屏幕支持清单页 =================
// 一张表把 epd_panel.h 里所有可用型号摊在屏上，当前型号反白。
// 驱动本身全部来自 GxEPD2，这里只列元数据，不含第三方固件代码。
static void drawPanelPage()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, W, 18, GxEPD_BLACK);

  char t[52];
  sprintf(t, "支持屏幕 %u 种 · 当前 %s %s", (unsigned)PANEL_COUNT, EPD_NAME, EPD_IC);
  cn(5, 14, t, F_SMALL, GxEPD_WHITE, false);

  // 两列排布：左列前 6 项，右列剩下的
  const int16_t lx = 4, rx = 150, y0 = 32, lh = 15;
  uint8_t half = (PANEL_COUNT + 1) / 2;

  for (uint8_t i = 0; i < PANEL_COUNT; i++) {
    uint8_t col = (i < half) ? 0 : 1;
    uint8_t row = (i < half) ? i : (i - half);
    int16_t x = col ? rx : lx;
    int16_t y = y0 + row * lh;

    const PanelEntry &p = PANEL_LIST[i];
    bool cur = (p.id == EPD_PANEL);

    if (cur) display.fillRect(x - 1, y - 11, 142, lh - 2, GxEPD_BLACK);

    char line[34];
    sprintf(line, "%s %s %s", p.colors == 3 ? "3C" : "BW", p.name, p.ic);
    cn(x + 2, y, line, F_SMALL, cur ? GxEPD_WHITE : GxEPD_BLACK, cur);

    // 支持快刷参数的型号右侧打个星
    if (p.fast) cn(x + 132, y, "*", F_SMALL, cur ? GxEPD_WHITE : GxEPD_BLACK, false);
  }
}

// ================= 刷新 =================
static void pinsOk()
{
  pinMode(PIN_CS, OUTPUT); pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_BUSY, INPUT); pinMode(PIN_DC, OUTPUT);
}

static void refreshFull(uint8_t temp)
{
  pinsOk();
#if EPD_FASTPAR
  display.epd2.setFastTemp(temp);       // 0x1A 温度覆写
  display.epd2.setUpdateMode(0xD7);     // 0x22 配温度用的波形
  display.epd2.setGateLines(0);         // 0x01 不做窗口裁剪
#else
  (void)temp;                           // 这块屏的驱动没有注入接口，走库默认
#endif

  uint32_t t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    if (page == P_CLOCK)     { drawClockStatic(); drawClockDyn(); }
    else if (page == P_MENU) drawMenu();
    else if (page == P_PANEL) drawPanelPage();
    else                     drawRead();
  } while (display.nextPage());
  lastMs = millis() - t0;
}

static void refreshClockDyn(uint8_t temp)
{
  pinsOk();
#if EPD_FASTPAR
  display.epd2.setFastTemp(temp);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(DYN_W);     // 只扫 144 条门线 → 3.0s
#else
  (void)temp;
#endif

  uint32_t t0 = millis();
  display.setPartialWindow(DYN_X, 0, DYN_W, H);
  display.firstPage();
  do { drawClockDyn(); } while (display.nextPage());
  lastMs = millis() - t0;
  partialCount++;
}

// ================= 行为 =================
static void enterPage(uint8_t p)
{
  page = p;
  if (p == P_CLOCK) { partialCount = 0; refreshFull(65); }
  else              { refreshFull(100); }
  lastClockTick = millis();
  autoStepMs = millis();
}

static void onShort()
{
  if (page == P_CLOCK) { Serial.println(F("[短按] 时钟页无动作")); return; }
  if (page == P_PANEL) {
    Serial.println(F("[短按] 屏幕清单 → 菜单"));
    enterPage(P_MENU);
    return;
  }
  if (page == P_MENU) {
    menuSel = (menuSel + 1) % NMENU;
    Serial.printf("[短按] 菜单 → %s\r\n", MENU[menuSel]);
    refreshFull(100);
  } else {
    readPage = (readPage % 12) + 1;
    Serial.printf("[短按] 阅读第 %u 页\r\n", readPage);
    refreshFull(100);
  }
  autoStepMs = millis();
}

static void onLong()
{
  if (page == P_CLOCK) {
    Serial.println(F("[长按] 时钟 → 菜单"));
    menuSel = 0; enterPage(P_MENU);
  } else if (page == P_MENU) {
    if (menuSel == 0)      { Serial.println(F("[长按] 进入 阅读")); enterPage(P_READ);  }
    else if (menuSel == 2) { Serial.println(F("[长按] 回到 时钟")); enterPage(P_CLOCK); }
    else if (menuSel == 4) { Serial.println(F("[长按] 屏幕支持清单")); enterPage(P_PANEL); }
    else                   { Serial.printf("[长按] %s 待做\r\n", MENU[menuSel]); }
  } else {
    Serial.println(F("[长按] 阅读 → 菜单"));
    menuSel = 0; enterPage(P_MENU);
  }
}

// ================= 主流程 =================
void setup()
{
  Serial.begin(115200);
  delay(400);

  pinMode(PIN_KEY3, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_KEY3), isrKey3, CHANGE);

  display.init(115200, true, 20, false);
  display.setRotation(3);

  u8f.begin(display);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);
  u8f.setBackgroundColor(GxEPD_WHITE);

  Serial.println(F(""));
  Serial.println(F("===== 横屏三色中文界面 v16 ====="));
  clockBegin();
  Serial.printf("[屏幕] 型号 %s  驱动 %s  %u 色  快刷参数注入 %s\r\n",
                EPD_NAME, EPD_IC, EPD_COLORS, EPD_FASTPAR ? "支持" : "不支持");
  Serial.printf("[屏幕] 适配表共 %u 种，改 epd_panel.h 的 EPD_PANEL 可切换\r\n",
                (unsigned)PANEL_COUNT);
  Serial.printf("竖带 %dpx -> 理论 %d ms\r\n", DYN_W, (int)(820 + 15.1 * DYN_W));

  // 字库自检（缺字会静默画空白，必须提前暴露）
  Serial.println(F("[字库自检] gb2312 全包"));
  fontCheck("年月日周三四五六日阅读天气钟设置关于温度湿名言占位节短按切换进入第页翻菜单支持屏幕种当前", F_SMALL);
  fontCheck("年月日周三四五六日阅读天", F_DATE);
  Serial.println(F("[字库自检] 符号"));
  fontCheck("℃%", F_SYM);

  pollSensors();
  enterPage(P_CLOCK);
  Serial.printf("首屏 %lu ms\r\n", (unsigned long)lastMs);
}

void loop()
{
  uint8_t ev = takeEvents();
  if (ev & 1) onShort();
  if (ev & 2) onLong();

  // 时钟页：每 5 分钟整屏刷一次
  //   黑白屏(A01 等)：库自带快刷 LUT，整屏约 1~2s，不需要温度注入
  //   三色屏：必须 65℃ 才能保住红色，整屏 11s 左右
  if (page == P_CLOCK && millis() - lastClockTick > CLOCK_FULL_MS) {
    lastClockTick = millis();
    pollSensors();
    ClockTime ct = clockRead();
    Serial.printf("[时钟] %02u:%02u 整屏刷新\r\n", ct.hour, ct.min);
#if EPD_COLORS == 3
    refreshFull(65);
#else
    refreshFull(0);
#endif
    Serial.printf("     -> %lu ms\r\n", (unsigned long)lastMs);
  }

  if (millis() - autoStepMs > 60000) {
    autoStepMs = millis();
    if (page != P_CLOCK) {
      Serial.println(F("[自动] 60s 无操作，回时钟页"));
      pollSensors();
      enterPage(P_CLOCK);
    }
  }

  delay(5);
}
