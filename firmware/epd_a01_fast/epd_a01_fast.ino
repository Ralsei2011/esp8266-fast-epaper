/*
 * A01 (SSD1608 / IL3820, 2.9" 黑白 128x296) —— 短脉冲快刷实验 v2
 *
 * 原理参考 TruElysia02/epd-fast (MIT) 公开文档所述思路，独立实现：
 *   1) 升压只开一次 0x22=0xC0，之后每次刷新跳过 power on/off（省 180ms）
 *   2) 每次只发 0x22=0x04（display），不重载 LUT、不重载温度
 *   3) IL3820 的 LUT 每字节 = 2bit 电平 + 6bit 时长，把时长压短，
 *      让"一次扫描"从 420ms 降到几毫秒
 *
 * 本固件逐档缩放 LUT 时序，测出「单次刷新」与「连续帧间隔」的真实极限。
 * 串口：'0'..'5' 切档，'a' 跑动画，'q' 关电退出。
 */

#include <GxEPD2_BW.h>

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4

GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT>
  display(GxEPD2_290(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define GATES    296
#define SOURCES  128
#define ROWB     (SOURCES / 8)      // 16
#define FBBYTES  (GATES * ROWB)     // 4736

static uint8_t fb[FBBYTES];

// IL3820 出厂局刷波形（GxEPD2 LUTDefault_part 的 30 字节数据部分）
static const uint8_t LUT_PART[30] = {
  0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x13, 0x14, 0x44, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 出厂全刷波形
static const uint8_t LUT_FULL[30] = {
  0x50, 0xAA, 0x55, 0xAA, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 诊断用：全 0（理论上不驱动任何像素、不耗时）
static const uint8_t LUT_ZERO[30] = { 0 };

static uint8_t lutBuf[30];

struct Tier { uint8_t num, den; const char *name; };
static const Tier TIERS[] = {
  { 1,  1, "1/1 " }, { 1,  2, "1/2 " }, { 1,  4, "1/4 " },
  { 1,  8, "1/8 " }, { 1, 16, "1/16" }, { 1, 32, "1/32" },
};
#define NTIER (sizeof(TIERS) / sizeof(TIERS[0]))

static uint8_t curTier = 0;

// 把每条 phase 的时长按 num/den 缩放，电平位（高 2 位）原样保留
static void makeLut(uint8_t num, uint8_t den)
{
  for (uint8_t i = 0; i < 30; i++) {
    uint8_t lv = LUT_PART[i] & 0xC0;
    uint16_t dur = LUT_PART[i] & 0x3F;
    if (dur) {
      uint16_t nd = (dur * num + den / 2) / den;
      if (nd == 0) nd = 1;
      if (nd > 63) nd = 63;
      lutBuf[i] = lv | (uint8_t)nd;
    } else {
      lutBuf[i] = lv;
    }
  }
}

// ---- 绘图（y = gate 0..295 横向，x = source 0..127 纵向）----
static void fbClear(uint8_t v) { memset(fb, v, FBBYTES); }

static void fbRect(int16_t g0, int16_t g1, int16_t s0, int16_t s1, uint8_t v)
{
  if (g0 < 0) g0 = 0;
  if (g1 > GATES - 1) g1 = GATES - 1;
  if (s0 < 0) s0 = 0;
  if (s1 > SOURCES - 1) s1 = SOURCES - 1;
  for (int16_t y = g0; y <= g1; y++) {
    uint8_t *row = fb + (uint32_t)y * ROWB;
    for (int16_t x = s0; x <= s1; x++) {
      uint8_t m = 0x80 >> (x & 7);
      if (v) row[x >> 3] |= m; else row[x >> 3] &= ~m;
    }
  }
}

static void drawTierMark(uint8_t n)
{
  for (uint8_t i = 0; i < n; i++) {
    int16_t g = 16 + i * 44;
    fbRect(g, g + 32, 24, 60, 0);        // 黑块
    fbRect(g + 8, g + 24, 32, 52, 1);    // 挖白 -> 检验对比度和边缘
  }
}

static void drawChecker(int16_t g0, int16_t s0, int16_t size, uint8_t cells)
{
  for (uint8_t a = 0; a < cells; a++)
    for (uint8_t b = 0; b < cells; b++)
      fbRect(g0 + a * size, g0 + a * size + size - 1,
             s0 + b * size, s0 + b * size + size - 1,
             ((a + b) & 1) ? 1 : 0);
}

// ---- 一圈：上传 + 短扫描 ----
static uint32_t lastUpUs = 0, lastScanUs = 0;

static uint32_t scanFrame()
{
  uint32_t t0 = micros();
  display.epd2.writeImage(fb, 0, 0, SOURCES, GATES, false, false, false);
  uint32_t t1 = micros();
  display.epd2.expScan();
  uint32_t t2 = micros();
  lastUpUs = t1 - t0;
  lastScanUs = t2 - t1;
  return t2 - t0;
}

static void enterTier(uint8_t t)
{
  curTier = t;
  makeLut(TIERS[t].num, TIERS[t].den);
  display.epd2.expInit();
  display.epd2.expSetLut(lutBuf, 30);
  display.epd2.expAnalogOn();
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("===== A01 (IL3820) 短脉冲快刷 v2 ====="));
  Serial.printf("帧 %d 字节, %d 档\n", FBBYTES, (int)NTIER);

  // 库初始化会做一次标准全刷，把屏刷成白色
  display.init(0, true, 2, false);
  // 默认只有 4MHz，上传 4736 字节要 26ms，提到 40MHz
  display.epd2.selectSPI(SPI, SPISettings(40000000, MSBFIRST, SPI_MODE0));
  Serial.println(F("[基准] 标准全刷完成，SPI 提到 40MHz"));

  // ============ 核心验证：短脉冲累积 ============
  // 假设：0x14 之后的每次扫描只有 ~1ms 的驱动剂量，单次肉眼不可见，
  // 需要像 epd-fast 那样重复累积。这里只上传一次图像，然后连刷 N 次。
  makeLut(1, 1);
  display.epd2.expInit();
  display.epd2.expSetLut(lutBuf, 30);

  // 先用标准 0x04 清成白底（这一步走 OTP，保证干净）
  fbClear(0xFF);
  display.epd2.writeImage(fb, 0, 0, SOURCES, GATES, false, false, false);
  display.epd2.expScanMode(0x04, 3000);

  // 画高对比图案：左侧大黑块 + 右侧棋盘
  fbClear(0xFF);
  fbRect(20, 130, 10, 110, 0);
  drawChecker(160, 20, 12, 8);
  display.epd2.writeImage(fb, 0, 0, SOURCES, GATES, false, false, false);

  Serial.println(F("[累积] 开始连续短脉冲，每 20 次打印一次"));
  uint32_t acc = 0, t0 = micros();
  for (uint16_t i = 1; i <= 200; i++) {
    uint32_t a = micros();
    display.epd2.expScanMode(0x14, 2000);
    acc += micros() - a;
    if (i % 20 == 0) {
      Serial.printf("  第 %3u 次  累计 %5lu us  平均 %lu us/次\r\n",
                    i, (unsigned long)acc, (unsigned long)(acc / i));
    }
  }
  Serial.printf("[累积] 200 次共 %lu ms\r\n", (unsigned long)((micros() - t0) / 1000));
  Serial.println(F(">>> 请看屏幕：左边黑块和右边棋盘是否已经显现？"));
  Serial.println(F(">>> 如果全白 -> 短脉冲没驱动像素；如果显现 -> 复刻成功"));

  // 故意不覆盖画面，让累积结果留在屏上供观察
}

void loop()
{
  if (!Serial.available()) return;
  char c = Serial.read();

  if (c == 'a') {
    Serial.println(F("[动画] 20 帧全屏横扫"));
    uint32_t sum = 0;
    for (uint8_t f = 0; f < 20; f++) {
      fbClear(0xFF);
      int16_t g = 8 + f * 14;
      fbRect(g, g + 14, 0, SOURCES - 1, 0);
      uint32_t dt = scanFrame();
      sum += dt;
    }
    Serial.printf("[动画] 均 %lu us -> %.1f fps\r\n",
                  (unsigned long)(sum / 20), 1e6 / (sum / 20.0));
  } else if (c == 'q') {
    display.epd2.expPowerOff();
    Serial.println(F("[关电] 屏保画面，长时间通电请按此退出"));
  }
}
