/*
 * 20px 粗体中文字库验证
 *
 * wqy16 点阵笔画 1px、间隙 1px，叠加加粗必然糊成一团。
 * 这里换成用「微软雅黑 Bold」渲染的 20px 点阵（cn_font.h），
 * 天生笔画 2~3px，不需要任何加粗。
 *
 * 屏上分三段：
 *   上：20px 粗体  "2026年11月11日"
 *   中：20px 粗体  "周三  58%  19℃"
 *   下：wqy16 老字  "2026年11月11日"（对照组，同样不加粗）
 *
 * 纯黑白 + 100℃：5.3 秒出结果，先确认字形本身够不够粗。
 */

#define ENABLE_GxEPD2_GFX 1

#include <GxEPD2_3C.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>
#include "cn_font.h"

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W  296
#define H  128

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8f;

#define F_WQY16  u8g2_font_wqy16_t_gb2312
#define F_TAG    u8g2_font_wqy12_t_gb2312

// ================= 20px 粗体字库绘制 =================
// 字库在 flash（PROGMEM），必须用 pgm_read_* 读取
static int16_t cnFind(uint16_t code)
{
  int16_t lo = 0, hi = CNF_COUNT - 1;
  while (lo <= hi) {
    int16_t mid = (lo + hi) >> 1;
    uint16_t c = pgm_read_word(&CNF_IDX[mid].code);
    if (c == code) return mid;
    if (c < code) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

static uint8_t cnWidthOf(int16_t idx)
{
  return pgm_read_byte(&CNF_IDX[idx].w);
}

// UTF-8 解码一个字符，返回字节数
static uint8_t utf8Next(const uint8_t **pp, uint16_t *code)
{
  const uint8_t *p = *pp;
  uint8_t n;
  if (p[0] < 0x80)             { *code = p[0]; n = 1; }
  else if ((p[0] & 0xE0) == 0xC0) { *code = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
  else if ((p[0] & 0xF0) == 0xE0) { *code = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
  else                          { *code = '?'; n = 4; }
  *pp = p + n;
  return n;
}

// 画一个字符，返回宽度
static int16_t cnChar(int16_t x, int16_t baseline, uint16_t code, uint16_t color)
{
  int16_t idx = cnFind(code);
  if (idx < 0) {
    // 缺字形：画空心方块，一眼能发现
    display.drawRect(x, baseline - CNF_BASE + 2, 14, CNF_H - 4, color);
    return 10;
  }
  uint8_t w = cnWidthOf(idx);
  const uint8_t *bits = CNF_BITS + (uint32_t)idx * CNF_CHARSIZE;
  int16_t top = baseline - CNF_BASE;
  for (uint8_t y = 0; y < CNF_H; y++) {
    const uint8_t *row = bits + (uint16_t)y * CNF_ROWBYTES;
    for (uint8_t xi = 0; xi < w; xi++) {
      if (pgm_read_byte(row + (xi >> 3)) & (0x80 >> (xi & 7)))
        display.drawPixel(x + xi, top + y, color);
    }
  }
  return w;
}

// 画整串，返回总宽
static int16_t cnText(int16_t x, int16_t baseline, const char *s, uint16_t color)
{
  int16_t x0 = x;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint16_t code;
    utf8Next(&p, &code);
    x += cnChar(x, baseline, code, color);
  }
  return x - x0;
}

static int16_t cnWidth(const char *s)
{
  int16_t w = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint16_t code;
    utf8Next(&p, &code);
    int16_t idx = cnFind(code);
    w += (idx >= 0) ? cnWidthOf(idx) : 10;
  }
  return w;
}

static void cnCenter(int16_t cx, int16_t baseline, const char *s, uint16_t color)
{
  cnText(cx - cnWidth(s) / 2, baseline, s, color);
}

static void tag(int16_t x, int16_t y, const char *s)
{
  u8f.setFont(F_TAG);
  u8f.setForegroundColor(GxEPD_BLACK);
  u8f.setCursor(x, y);
  u8f.print(s);
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
  Serial.println(F("===== 20px 粗体字库验证 ====="));
  Serial.printf("字库 %d 字 / %d 字节  画布高 %d 基线 %d\r\n",
                CNF_COUNT, (int)sizeof(CNF_BITS), CNF_H, CNF_BASE);
  Serial.printf("宽度实测: 日期=%d  周三=%d  温湿度=%d\r\n",
                cnWidth("2026年11月11日"), cnWidth("周三"), cnWidth("58% 19℃"));

  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // 段1：20px 粗体日期（居中）
    cnCenter(W / 2, 22, "2026年11月11日", GxEPD_BLACK);

    // 段2：20px 粗体 周几 + 温湿度
    cnText(20, 52, "周三", GxEPD_BLACK);
    cnText(120, 52, "58%", GxEPD_BLACK);
    cnText(210, 52, "19℃", GxEPD_BLACK);

    display.drawFastHLine(4, 62, W - 8, GxEPD_BLACK);

    // 段3：老字库对照组（wqy16，不加粗）
    tag(4, 74, "wqy16:");
    u8f.setFont(F_WQY16);
    u8f.setForegroundColor(GxEPD_BLACK);
    u8f.setCursor(46, 86);
    u8f.print("2026年11月11日");
    u8f.setCursor(46, 108);
    u8f.print("周三 58% 19℃");

    // 段4：20px 粗体再画一遍小字行的对照组
    tag(4, 122, "20px:");
    cnText(40, 120, "周三 58% 19℃", GxEPD_BLACK);
  } while (display.nextPage());

  Serial.printf("完成 %lu ms\r\n", (unsigned long)millis());
}

void loop()
{
  delay(1000);
}
