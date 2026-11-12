/*
 * 加粗诊断 · 纯黑白版
 *
 * 关键：上两版都是红色字 + 65℃ 波形，照片显示"加了粗的地方变成浅红重影"。
 *       这里把颜色换成纯黑、把温度换成 100℃（最快、黑色驱动最足），
 *       如果黑白版加粗干净利落 → 问题在红色驱动，与加粗代码无关。
 *       如果黑白版也有重影 → 问题在多次 print 的绘制方式。
 *
 * 顺便用实测宽度排版，避免符号与数字重叠（上版重叠了 4px）。
 */

#define ENABLE_GxEPD2_GFX 1

#include <GxEPD2_3C.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W  296
#define H  128

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8f;

#define F_CN     u8g2_font_wqy16_t_gb2312
#define F_TAG    u8g2_font_wqy12_t_gb2312
#define F_NUM    u8g2_font_fub20_tn
#define F_LAT17  u8g2_font_fub17_tr

static void glyph(int16_t x, int16_t y, const char *s, uint8_t strength)
{
  u8f.setCursor(x, y); u8f.print(s);
  if (strength == 0) return;
  u8f.setCursor(x + 1, y); u8f.print(s);
  u8f.setCursor(x, y + 1); u8f.print(s);
  u8f.setCursor(x + 1, y + 1); u8f.print(s);
  if (strength >= 2) {
    u8f.setCursor(x - 1, y); u8f.print(s);
    u8f.setCursor(x, y - 1); u8f.print(s);
  }
  if (strength >= 3) {
    u8f.setCursor(x - 1, y - 1); u8f.print(s);
    u8f.setCursor(x + 1, y - 1); u8f.print(s);
    u8f.setCursor(x - 1, y + 1); u8f.print(s);
  }
}

// 逐字 + 字距，左对齐
static void spaced(int16_t x, int16_t y, const char *s, const uint8_t *font,
                   uint16_t color, uint8_t strength, uint8_t extra)
{
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    const uint8_t *st = p;
    if (*p < 0x80)                p += 1;
    else if ((*p & 0xE0) == 0xC0) p += 2;
    else if ((*p & 0xF0) == 0xE0) p += 3;
    else                          p += 4;
    char buf[6]; uint8_t k = (uint8_t)(p - st); if (k > 5) k = 5;
    memcpy(buf, st, k); buf[k] = 0;
    int16_t w = u8f.getUTF8Width(buf);
    glyph(x, y, buf, strength);
    x += w + extra;
  }
}

// 左对齐画，返回右边缘 x（用实测宽度，绝不硬编码）
static int16_t drawAt(int16_t x, int16_t y, const char *s,
                      const uint8_t *font, uint16_t color, uint8_t strength)
{
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  glyph(x, y, s, strength);
  return x + u8f.getUTF8Width(s);
}

void setup()
{
  Serial.begin(115200);
  delay(400);

  display.init(115200, true, 20, false);
  display.setRotation(3);

  u8f.begin(display);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);
  u8f.setBackgroundColor(GxEPD_WHITE);

  Serial.println(F(""));
  Serial.println(F("===== 加粗诊断 · 纯黑白 ====="));

  // 100℃：黑色驱动最足、最快（满屏约 5.3s）
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // 三档加粗，同一行字，左对齐，方便直接比粗细
    spaced(24, 20, "2026年11月11日", F_CN, GxEPD_BLACK, 0, 2);
    drawAt(8, 20, "0", F_TAG, GxEPD_BLACK, 0);

    spaced(24, 40, "2026年11月11日", F_CN, GxEPD_BLACK, 1, 2);
    drawAt(8, 40, "1", F_TAG, GxEPD_BLACK, 0);

    spaced(24, 60, "2026年11月11日", F_CN, GxEPD_BLACK, 2, 3);
    drawAt(8, 60, "2", F_TAG, GxEPD_BLACK, 0);

    spaced(24, 80, "2026年11月11日", F_CN, GxEPD_BLACK, 3, 4);
    drawAt(8, 80, "3", F_TAG, GxEPD_BLACK, 0);

    display.drawFastHLine(4, 88, W - 8, GxEPD_BLACK);

    // 下半：数字 + 符号，全部按实测宽度依次排，绝不重叠
    int16_t x = 8;
    x = drawAt(x, 112, "58", F_NUM, GxEPD_BLACK, 1);
    x = drawAt(x + 3, 112, "%", F_LAT17, GxEPD_BLACK, 0);
    x = drawAt(x + 14, 112, "19", F_NUM, GxEPD_BLACK, 1);
    x = drawAt(x + 3, 112, "°C", F_LAT17, GxEPD_BLACK, 0);

    // 右侧对照：wqy 的小符号（旧方案，8px 的 %）
    drawAt(190, 100, "wqy:", F_TAG, GxEPD_BLACK, 0);
    int16_t x2 = 218;
    x2 = drawAt(x2, 112, "58", F_NUM, GxEPD_BLACK, 1);
    x2 = drawAt(x2 + 2, 112, "%", F_CN, GxEPD_BLACK, 1);
    x2 = drawAt(x2 + 8, 112, "19", F_NUM, GxEPD_BLACK, 1);
    drawAt(x2 + 2, 112, "℃", F_CN, GxEPD_BLACK, 1);
  } while (display.nextPage());

  Serial.printf("黑白对照屏完成 %lu ms\r\n", (unsigned long)millis());
}

void loop()
{
  delay(1000);
}
