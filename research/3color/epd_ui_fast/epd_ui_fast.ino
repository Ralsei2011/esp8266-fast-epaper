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

// 物理帧：宽 128、高 296。expSetRamArea/expWriteRam 用的是物理坐标，
// 千万别拿横屏逻辑的 W/H 去传 —— 会把 X 地址写越界（36 > 15），数据全丢。
#define PW 128
#define PH 296

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
// ================= 按键 =================
// 这块板只有两个可编程键，第三个是 RESET（硬件复位，读不到）：
//   KEY_M 中键 = GPIO0，与屏的 DC 共用 —— 读之前必须临时切成输入，读完立刻还回去
//   KEY_R 右键 = GPIO3，也就是 RX 脚 —— Serial 只开 TX，把 RX 让出来
//
// 甘草酸原厂固件是三键（中 key_zj=GPIO0 / 右 key_yb=GPIO5），我们板上 GPIO5 给了 SD，
// 所以把三键语义折成两键：
//   短按中键 = 上移 / 上一项          （原「第 2 个键往左、往上」）
//   短按右键 = 下移 / 下一项          （原「第 3 个键往下、往右」）
//   长按右键 = 确认                   （原「长按第 3 个键」）
//   长按中键 = 返回上一页 / 回主页     （原「长按第 2 个键」）
// 时钟页是首页：短按中键即打开菜单（充当「按键一」），长按右键也能进。
#define KEY_M   PIN_DC
#define KEY_R   PIN_KEY3

#define KEY_LONG_MS     600
#define KEY_DEBOUNCE_MS 25

// 事件位：1=中键短按 2=中键长按 4=右键短按 8=右键长按
static uint8_t  keyEvents = 0;
static bool     kmPrev = false, krPrev = false;
static bool     kmLong = false, krLong = false;
static bool     kmLock = false, krLock = false;   // 长按只出一次，松开才复位
static uint32_t kmDown = 0, krDown = 0;

static bool readKeyM()
{
  pinMode(KEY_M, INPUT_PULLUP);
  delayMicroseconds(30);
  bool v = (digitalRead(KEY_M) == LOW);
  pinMode(KEY_M, OUTPUT);        // 立刻还给 DC，否则下一次刷新全被当成命令
  return v;
}

// GPIO3 是 UART 的 RX 脚，UART 会周期性把引脚模式抢回去、内部上拉随之失效，
// 悬空就恒定读成低电平（表现为「右键一直按着」）。所以每次读之前都要重设上拉。
static bool readKeyR()
{
  pinMode(KEY_R, INPUT_PULLUP);
  return digitalRead(KEY_R) == LOW;
}

// 重量级刷新会阻塞轮询几秒，期间状态不可信，刷新完强制重新采样
static void keyResync() { kmPrev = krPrev = false; kmLong = krLong = false; }

// 卡死保护：某个键连续按住超过 10 秒，判定为线路异常，不再产生事件
#define KEY_STUCK_MS 10000UL

static void pollKeys()
{
  // 卡死保护：某个键连续按住超过 10 秒，判定为线路异常，不再产生事件
  #define KEY_STUCK_MS 10000UL
  static bool kStuck = false;

  bool m = readKeyM();
  bool r = readKeyR();
  uint32_t now = millis();

  if (r && krDown && now - krDown > KEY_STUCK_MS) {
    if (!kStuck) { Serial.println(F("[键] K3 持续为低，判定线路异常，忽略")); kStuck = true; }
    r = false;
  } else if (!r) {
    kStuck = false;
  }

  // 电平诊断：只在变化时打印，用来区分「硬件没读到」和「软件判定错」
  static bool dbgM = false, dbgR = false;
  if (m != dbgM || r != dbgR) {
    Serial.printf("[电平] K0=%d K3=%d\r\n", m ? 1 : 0, r ? 1 : 0);
    dbgM = m; dbgR = r;
  }

  // 「长按锁」：长按事件只允许出一次，必须等手松开才复位。
  // 否则每次刷新后的 keyResync() 都会把长按标志清掉，手一直按着就会
  // 每 600ms 重复触发一次（实测按住右键 2 秒会连续「确认」3 次）。
  if (!m) kmLock = false;
  if (!r) krLock = false;

  if (!kmLock) {
    if (m && !kmPrev)   { kmDown = now; kmLong = false; }
    else if (m)         { if (!kmLong && now - kmDown >= KEY_LONG_MS) { kmLong = true; kmLock = true; keyEvents |= 2; } }
    else if (kmPrev)    { if (!kmLong && now - kmDown >= KEY_DEBOUNCE_MS) keyEvents |= 1; }
  }
  kmPrev = m;

  if (!krLock) {
    if (r && !krPrev)   { krDown = now; krLong = false; }
    else if (r)         { if (!krLong && now - krDown >= KEY_LONG_MS) { krLong = true; krLock = true; keyEvents |= 8; } }
    else if (krPrev)    { if (!krLong && now - krDown >= KEY_DEBOUNCE_MS) keyEvents |= 4; }
  }
  krPrev = r;
}

static uint8_t takeEvents()
{
  uint8_t e; noInterrupts(); e = keyEvents; keyEvents = 0; interrupts(); return e;
}

// ================= 状态 =================
enum { P_CLOCK = 0, P_MENU, P_READ, P_PANEL };
static uint8_t  page = P_CLOCK;
static uint8_t  cHour = 12, cMin = 34;
static uint8_t  menuSel = 0;
static uint8_t  readPage = 1;
static uint16_t partialCount = 0;
static uint8_t  lastMinShown = 255;   // 上次显示到屏上的分钟，变化时才刷
static uint32_t lastMs = 0;
static uint32_t autoStepMs = 0;
static uint32_t lastClockTick = 0;
static uint32_t lastMinCheck = 0;

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

// 当前页的全部绘制（各刷新通道共用）
static void drawPage()
{
  if (page == P_CLOCK)      { drawClockStatic(); drawClockDyn(); }
  else if (page == P_MENU)  drawMenu();
  else if (page == P_PANEL) drawPanelPage();
  else                      drawRead();
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
  do { drawPage(); } while (display.nextPage());
  lastMs = millis() - t0;
}

// ---- 92ms 快刷通道 --------------------------------------------------------
// 0xC8 = 0x80(时钟) + 0x40(电源) + 0x08(Mode 2)，不含 0x10(载 OTP 表)，
// 直接用 OTP 里烧好的 Mode 2 黑白波形，整屏 90~92ms（12 帧 × 296 行）。
//
// 两个前提，缺一个画面就错：
//   1. 要画的内容里不能有红（_color buffer 必须全 0，红平面才会全非红）
//   2. 屏上不能有残留的红色粒子 —— Mode 2 只驱动黑白，红粒不动，会原地留着
// 菜单/阅读/清单页天然满足 1；第 2 条靠进页前的 washRed() 保证。
// GxEPD2 上传 0x26 时做的是 _writeData(~color)：菜单页无红（buffer 全 1），
// 取反后整个平面是 0x00 = 非红，正好是 Mode 2 要的状态，不需要额外处理。
//
// 0xC8（Mode 2）在这条路径上实测无效：波形照跑 94ms、BUSY 正常释放、
// 但屏幕上什么都不动（换过 0xF8 前置、0x26 归零都没有改善）。
// 0xC4 是 OTP 的 Mode 1 标准波形，实测 2.95 秒，能正常出画。
// 153 字节黑白波形表，与 epd_vm 里五屏验证过的那张完全一致
// （结构同 GxEPD2_213_BN::lut_partial，GPL-3.0-or-later）
static const uint8_t LUT_BW[153] = {
  0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

// 0x26 全 0 缓冲：GxEPD2 上传时写的是 ~color（菜单页 color buffer 全 0 -> 写出 0xFF =
// 整屏红），必须覆盖成 0x00 才是"非红"。
static uint8_t fastZeros[4736] __attribute__((aligned(4)));

// 快刷触发：直接 0xC8。不需要 0xC0/0xF8 上电，也不需要 0x32 写 LUT。
// research/3color/bwfast.md 实测：0x32 写什么、写不写，都是 92ms —— 这颗三色
// SSD1680 根本不响应自定义 LUT，92ms 来自 OTP 里预烧好的 Mode 2 波形。
static void fastFire()
{
  display.epd2.expSetRamArea(0, 0, PW, PH);
  display.epd2.expWriteRam(0x26, fastZeros, sizeof(fastZeros));   // 红平面全 0（1=红）
  display.epd2.expActivate(0xC8, 20000);                         // Mode 2
}

// 裸上传：自己维护物理帧缓冲，expWriteRam 直接写 RAM，完全不经过 GxEPD2。
// bwfast3 就是这条路跑出画面的；它和 writeImage 的差别是后者内部会发 SWRESET
// 并重设窗口，很可能把数据写到了别处。
static uint8_t rawBW[4736] __attribute__((aligned(4)));

static void paintCheckerRaw()
{
  memset(rawBW, 0xFF, sizeof(rawBW));              // 1 = 白
  for (uint16_t y = 0; y < PH; y++)
    for (uint16_t x = 0; x < PW; x++) {
      if (((x / 24) + (y / 24)) & 1) continue;     // 白格跳过
      rawBW[y * (PW / 8) + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));   // 0 = 黑
    }
  display.epd2.expSetRamArea(0, 0, PW, PH);
  display.epd2.expWriteRam(0x24, rawBW, sizeof(rawBW));
  display.epd2.expWriteRam(0x26, fastZeros, sizeof(fastZeros));
  display.epd2.expActivate(0xC8, 20000);
}

// 快刷：三步缺一不可。
//   ① 0xF8 上电 + 载 OTP 波形表
//   ② 0x32 覆盖 153 字节自定义 LUT   <-- 漏掉这步，时序照跑、BUSY 照释放、墨一点不动
//   ③ 0xC8 触发 Mode 2
static void refreshFast()
{
  pinsOk();
  uint32_t t0 = millis();
#if EPD_FASTPAR
  display.epd2.setFastTemp(0);          // 不写 0x1A
  display.epd2.setUpdateMode(0xC8);     // 这一遍只负责把数据送进 RAM
  display.epd2.setGateLines(0);
#endif

  display.setFullWindow();
  display.firstPage();
  do { drawPage(); } while (display.nextPage());

#if EPD_FASTPAR
  fastFire();
#endif
  lastMs = millis() - t0;
}

// 清红：画全白 + 三色波形（0xD7/100℃），把红色粒子推回白，顺带清残影。
// 代价约 5.1 秒。红色是「脏状态」，用过之后要回黑白必须先付这笔清洗费。
static void washRed()
{
  pinsOk();
#if EPD_FASTPAR
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(0);
#endif

  uint32_t t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
  lastMs = millis() - t0;
}

// 时钟页动态刷新：右半 144px 是纯黑白（时间/电量），左半 152px 是红色静态区。
//
// 关键点：Mode 2（0xC8）只驱动黑白粒子，红色粒子完全不参与。
// 所以整屏走 0xC8 时，左边那半的红字会原样留在原地，只有右边的黑白内容被更新。
// 92ms，比原来「把 GDR 砍到 144 门线 ≈ 3 秒」快 33 倍。
//
// 但红色粒子长期不被任何波形驱动，可能会发淡，所以每 120 次插一次三色全刷补红。
static uint16_t dynCount = 0;

static void refreshClockDyn(uint8_t temp)
{
  (void)temp;
  dynCount++;
  if (dynCount % 120 == 0) {
    Serial.println(F("[补红] 动态刷新满 120 次，走三色波形把红补实"));
    refreshFull(65);
    return;
  }
  refreshFast();
}

// ================= 行为 =================
static void enterPage(uint8_t p)
{
  bool leavingClock = (page == P_CLOCK);
  page = p;
  if (p == P_CLOCK) {
    partialCount = 0;
    refreshFull(65);                    // 时钟页有红，必须三色波形，约 11s
  } else {
    // 黑白页面。离开时钟页时屏上带着红，必须用三色波形推一次；
    // 但不必「先刷白再刷目标页」——直接三色刷目标页，一次到位：
    // 既把红推掉，又画出菜单，省掉一次 5 秒白屏和一次残影积累。
    if (leavingClock) {
      Serial.println(F("[切换] 时钟带红，用三色波形直接刷新页（约 5.1s）"));
      refreshFull(100);
    } else {
      refreshFast();
    }
  }
  Serial.printf("[页面] %u  刷新 %lu ms\r\n", p, (unsigned long)lastMs);
  keyResync();                        // 刷新期间阻塞，按键状态要重新采样
  if (p == P_CLOCK) lastMinShown = clockRead().min;   // 刚刷过，别立刻再刷一次
  lastClockTick = millis();
  autoStepMs = millis();
}

// id: 0 = 中键（上移 / 返回）  1 = 右键（下移 / 确认）
static void onKeyShort(uint8_t id)
{
  if (page == P_CLOCK) {
    if (id == 0) { Serial.println(F("[中短] 时钟 -> 菜单")); menuSel = 0; enterPage(P_MENU); }
    else         { Serial.println(F("[右短] 时钟页无动作（长按右键进菜单）")); }
    return;
  }

  if (page == P_MENU) {
    if (id == 0) { menuSel = (menuSel + NMENU - 1) % NMENU; Serial.printf("[中短] 上移 -> %s\r\n", MENU[menuSel]); }
    else         { menuSel = (menuSel + 1) % NMENU;         Serial.printf("[右短] 下移 -> %s\r\n", MENU[menuSel]); }
    refreshFast();
    Serial.printf("    -> %lu ms\r\n", (unsigned long)lastMs);
    return;
  }

  if (page == P_READ) {
    if (id == 0) readPage = (readPage <= 1) ? 12 : (uint8_t)(readPage - 1);
    else         readPage = (readPage % 12) + 1;
    Serial.printf("[%s] 阅读第 %u 页\r\n", id ? "右短" : "中短", readPage);
    refreshFast();
    Serial.printf("    -> %lu ms\r\n", (unsigned long)lastMs);
    return;
  }

  Serial.println(F("[键] 清单页 -> 回菜单"));
  enterPage(P_MENU);
}

static void onKeyLong(uint8_t id)
{
  if (id == 1) {                                  // 右键长按 = 确认
    if (page == P_CLOCK) { menuSel = 0; enterPage(P_MENU); return; }
    if (page == P_MENU) {
      if (menuSel == 0)      { Serial.println(F("[确认] 阅读"));      enterPage(P_READ);  }
      else if (menuSel == 2) { Serial.println(F("[确认] 时钟"));      enterPage(P_CLOCK); }
      else if (menuSel == 4) { Serial.println(F("[确认] 屏幕清单"));  enterPage(P_PANEL); }
      else                   { Serial.printf("[确认] %s 待做\r\n", MENU[menuSel]); }
    }
    return;
  }

  // 中键长按 = 返回上一页
  if (page == P_MENU)  { Serial.println(F("[中长] 菜单 -> 时钟")); enterPage(P_CLOCK); return; }
  if (page == P_READ)  { Serial.println(F("[中长] 阅读 -> 菜单")); enterPage(P_MENU);  return; }
  if (page == P_PANEL) { Serial.println(F("[中长] 清单 -> 菜单")); enterPage(P_MENU);  return; }

  Serial.println(F("[中长] 时钟页整屏重刷"));
  refreshFull(65);
}

// ================= 主流程 =================
// 串口自检：发 't' 跑一遍 时钟→菜单→阅读→清单→时钟，每步报耗时。
// 用来在没有按键的情况下量化各通道，也方便换屏后快速确认。
// 模式观察测试 v3：慢节奏，一次只看一个变化点。
// 每轮节奏 = 刷全白(5.1s) -> 停 12s -> 0xF8 刷棋盘(0.2s) -> 停 20s
// 屏幕上每 32 秒只有一次变化，不可能看漏。
static void drawChecker()
{
  display.fillScreen(GxEPD_WHITE);
  for (int16_t y = 0; y < (int16_t)H; y += 24)
    for (int16_t x = 0; x < (int16_t)W; x += 24)
      if (((x / 24) + (y / 24)) & 1) display.fillRect(x, y, 24, 24, GxEPD_BLACK);
}

static void paintWhite()
{
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(0);
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
}

static void paintCheckerFast()
{
  display.epd2.setFastTemp(0);
  display.epd2.setUpdateMode(0xC8);
  display.epd2.setGateLines(0);
  display.setFullWindow();
  display.firstPage();
  do { drawChecker(); } while (display.nextPage());
  fastFire();
}

static void selftest()
{
  Serial.println(F("===== 裸上传 白/格交替 ====="));
  for (uint8_t r = 1; r <= 12; r++) {
    pinsOk();
    paintWhite();
    Serial.printf("[%u] 白 (6s)\r\n", r);
    delay(6000);

    uint32_t t0 = millis();
    paintCheckerRaw();
    Serial.printf("[%u] 格 (6s) %lu ms\r\n", r, (unsigned long)(millis() - t0));
    delay(6000);
  }
  Serial.println(F("===== 结束 ====="));
}

void setup()
{
  // ★ 必须用默认 Serial.begin：GPIO3 是 UART RX 脚，
  //   用 SERIAL_TX_ONLY 显式指定 TX 引脚后，digitalRead(3) 会一直读到 UART
  //   的空闲高电平，按键永远读不到（实测 K3 恒为 1）。
  //   默认模式下 UART 虽然占着 RX，但 GPIO 输入寄存器仍反映引脚真实电平。
  Serial.begin(115200);
  delay(400);

  // 两个键都是低电平有效，内部上拉。GPIO0 平时是 DC(输出)，读键时才临时切输入。
  pinMode(KEY_R, INPUT_PULLUP);

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

  // ★ 关键诊断：UART 是否吃掉了 GPIO3 的内部上拉？
  //   关掉串口再读一次，如果 K3 变高，说明「关串口」就能救右键；
  //   如果仍是低，说明是硬件层面（按键粘连/缺上拉电阻），软件无解。
  Serial.println(F("[诊断] Serial.end() 后重读两键电平"));
  Serial.flush();
  Serial.end();
  pinMode(KEY_R, INPUT_PULLUP);
  pinMode(KEY_M, INPUT_PULLUP);
  delay(150);
  bool k3free = digitalRead(KEY_R);
  bool k0free = digitalRead(KEY_M);
  pinMode(KEY_M, OUTPUT);
  Serial.begin(115200);
  delay(250);
  Serial.printf("[诊断] 串口关闭时 K3=%d K0=%d   (1=高/未按, 0=低/按下)\r\n",
                k3free ? 1 : 0, k0free ? 1 : 0);
  Serial.flush();

  // V15 原厂逻辑：按住右键（按键3）再按复位键，直接进模式选择界面。
  // 实现方式就是开机第一时间读 GPIO3 —— 按住时它是低电平。
  // 上电自检开关。1 = 每次上电先跑「白/格交替循环」，用来验证刷新通道能不能
  // 真的出画（0xC8 那条路在不同屏上结果不一样，详见 research/3color/ui-clock.md）。
  // 正常使用时保持 0。
#ifndef BOOT_SELFTEST
#define BOOT_SELFTEST 0
#endif
#if BOOT_SELFTEST
  selftest();
#endif

  bool bootMenu = (digitalRead(KEY_R) == LOW);
  if (bootMenu) {
    Serial.println(F("[启动] 检测到右键按住 -> 直接进菜单"));
    menuSel = 0;
    enterPage(P_MENU);
  } else {
    enterPage(P_CLOCK);
  }
  Serial.printf("首屏 %lu ms\r\n", (unsigned long)lastMs);
}

void loop()
{
  pollKeys();

  // 串口命令只在开机前 60 秒内有效：GPIO3 兼作 RX，按键会在串口上产生噪声字节，
  // 长时间开着容易被误触发。
  if (Serial.available() && millis() < 180000) {
    char c = Serial.read();
    if (c == 't') selftest();
    else if (c == 'm') { menuSel = 0; enterPage(P_MENU); }
    else if (c == 'c') { enterPage(P_CLOCK); }
    else if (c == 'p') { enterPage(P_PANEL); }
  }

  uint8_t ev = takeEvents();
  if (ev & 1) { onKeyShort(0); keyResync(); autoStepMs = millis(); }
  if (ev & 4) { onKeyShort(1); keyResync(); autoStepMs = millis(); }
  if (ev & 2) { onKeyLong(0);  keyResync(); autoStepMs = millis(); }
  if (ev & 8) { onKeyLong(1);  keyResync(); autoStepMs = millis(); }

  // 时钟页：分钟一变就用 92ms 快刷（红色粒子不动，只更新右半的黑白内容）
  if (page == P_CLOCK && millis() - lastMinCheck > 1000) {
    lastMinCheck = millis();
    ClockTime ct0 = clockRead();
    if (ct0.min != lastMinShown) {
      lastMinShown = ct0.min;
      pollSensors();
      Serial.printf("[时钟] %02u:%02u 动态刷新\r\n", ct0.hour, ct0.min);
      refreshClockDyn(65);
      Serial.printf("     -> %lu ms\r\n", (unsigned long)lastMs);
      keyResync();
    }
  }

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
    keyResync();
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
