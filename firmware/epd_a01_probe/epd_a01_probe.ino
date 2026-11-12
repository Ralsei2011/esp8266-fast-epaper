/*
 * A01 (IL3820 / SSD1608, 2.9" 128x296 黑白) —— 全量波形探测 v4
 *
 * v3 已经证实的结论（31 档实测，日志 logs/probe_*.log）：
 *   1) IC 完全接受我们 0x32 写的 30 字节 LUT，刷新耗时严格 = sum(TP) × TFRAME
 *      sumTP=20 -> 413.4ms, 12 -> 248.0ms, 8 -> 165.3ms, 120 -> 2480ms, 0 -> 7us
 *      拟合 TFRAME = 20.67ms
 *   2) 0x22 的 bit4(0x10) 是「Load LUT」= 从 OTP 按温度选 WS 覆盖寄存器，
 *      这块屏的 OTP WS 全是空的 → 一调 0x14 就变 123us，之后 LUT 寄存器被污染。
 *      温度写 0/10/20/25/30/40/50/60°C 全是 123us → OTP 这条路废了，别指望它。
 *   3) 之前六轮测不出来的真正原因：库的 writeImage() 在 !_using_partial_mode 时
 *      会偷偷调 _Init_Part()，把 LUTDefault_part 重新写回去，覆盖掉我们的波形。
 *      现在改用 expUploadImage() 只写 RAM，问题消失。
 *
 * v4 要解决的：TFRAME 能不能降？
 *   数据手册 p32：
 *     0x3B Gate line width  TGate: 0000=30us 0001=34 0010=38 0011=40 0100=44
 *                                  0101=46 0110=52 0111=56 1000=62[POR] 1001=68
 *                                  1010=78 1011=88 1100=104 1101=125 1110=156 1111=208
 *     0x3A Dummy line 数量：单位 = TGate，POR=0x02，可设 0..127
 *   → TFRAME ≈ (门线数 + dummy) × TGate
 *     实测 (296+26)×62us = 19.96ms ≈ 20.67ms 拟合值
 *   → 理论上 (296+0)×30us = 8.88ms，若再砍 MUX 还能更低
 *
 * 21 档，每档一行 CSV：
 *   C,<序号>,<名字>,<phases>,<sumTP>,<gate>,<dummy>,<mux>,<total_us>,<tframe_us>,<busy>,<edges>
 *
 * 屏上读数：每档只画一根竖条，位置 x = 2 + 档序号*5。
 * 刷新真的驱动了像素 → 竖条跳到新位置；空转 → 原地不动。
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
#define ROWB     (SOURCES / 8)
#define FBBYTES  (GATES * ROWB)

static uint8_t fb[FBBYTES];
static uint8_t lut[30];
static uint8_t gTpSum = 0;

// 出厂局刷波形的相位电平（7 个驱动相：0x10,0x18,0x18,0x08,0x18,0x18,0x08）
static const uint8_t PH_PART[7] = { 0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08 };

// ---------------------------------------------------------------- LUT 构造

static void tpSum()
{
  uint8_t s = 0;
  for (uint8_t k = 0; k < 10; k++) s += (lut[20 + k] & 0x0F) + (lut[20 + k] >> 4);
  gTpSum = s;
}

// 用出厂 7 个驱动相，前 n 个相位各给 dur，其余相位 0
static void lutBuild(uint8_t n, uint8_t dur)
{
  memset(lut, 0, 30);
  for (uint8_t p = 0; p < n && p < 7; p++) lut[p] = PH_PART[p];
  for (uint8_t p = 0; p < 20; p++) {
    uint8_t tp = (p < n) ? dur : 0;
    uint8_t k = p >> 1;
    if (p & 1) lut[20 + k] |= (uint8_t)((tp & 0x0F) << 4);
    else       lut[20 + k] |= (uint8_t)(tp & 0x0F);
  }
  tpSum();
}

// ---------------------------------------------------------------- 画面

static void fbClear(uint8_t v) { memset(fb, v, FBBYTES); }

static void fbBar(int16_t x, uint8_t w)
{
  if (x < 0) return;
  for (int16_t y = 0; y < GATES; y++) {
    uint8_t *row = fb + (uint32_t)y * ROWB;
    for (int16_t i = 0; i < w; i++) {
      int16_t s = x + i;
      if (s < 0 || s >= SOURCES) continue;
      row[s >> 3] &= (uint8_t)~(0x80 >> (s & 7));
    }
  }
}

static void drawCaseBar(uint8_t idx)
{
  fbClear(0xFF);
  int16_t x = 2 + (int16_t)idx * 5;
  if (x > SOURCES - 4) x = SOURCES - 4;
  fbBar(x, 3);
}

// ---------------------------------------------------------------- 档位表

struct Case {
  const char *name;
  uint8_t  phases;    // 用几个驱动相
  uint8_t  dur;       // 每个相位的 TP
  int8_t   gate;      // 0x3B TGate 码值，-1 不改
  int8_t   dummy;     // 0x3A dummy 数量，-1 不改
  uint16_t mux;       // 0 = 不改
  uint8_t  repeat;    // >1 则连刷这么多次
  bool     drawBar;
};

static const Case CASES[] = {
  // ---- A TGate 扫描（dummy=0, mux 保持 296, sumTP=7）----
  { "TGate8-d0",    7, 1,  8, 0,   0, 1, true },   // POR 档，对照
  { "TGate6-d0",    7, 1,  6, 0,   0, 1, true },
  { "TGate4-d0",    7, 1,  4, 0,   0, 1, true },
  { "TGate2-d0",    7, 1,  2, 0,   0, 1, true },
  { "TGate1-d0",    7, 1,  1, 0,   0, 1, true },
  { "TGate0-d0",    7, 1,  0, 0,   0, 1, true },   // 30us 最小档

  // ---- B DummyLine 扫描（TGate=0）----
  { "d26-g0",       7, 1,  0, 26,  0, 1, true },   // POR dummy
  { "d8-g0",        7, 1,  0, 8,   0, 1, true },
  { "d2-g0",        7, 1,  0, 2,   0, 1, true },
  { "d0-g0",        7, 1,  0, 0,   0, 1, true },

  // ---- C MUX（TGate=0, dummy=0）----
  { "mux296-g0",    7, 1,  0, 0, 296, 1, true },
  { "mux148-g0",    7, 1,  0, 0, 148, 1, true },
  { "mux64-g0",     7, 1,  0, 0,  64, 1, true },
  { "mux32-g0",     7, 1,  0, 0,  32, 1, true },
  { "mux16-g0",     7, 1,  0, 0,  16, 1, true },

  // ---- D sumTP 下限（TGate=0, dummy=0, mux=296）----
  { "sumTP1",       1, 1,  0, 0, 296, 1, true },
  { "sumTP2",       2, 1,  0, 0, 296, 1, true },
  { "sumTP4",       4, 1,  0, 0, 296, 1, true },
  { "sumTP7",       7, 1,  0, 0, 296, 1, true },
  { "sumTP14",      7, 2,  0, 0, 296, 1, true },

  // ---- E 连刷稳定性（最快组合连刷 20 帧）----
  { "repeat20",     4, 1,  0, 0, 296, 20, false },
};

#define NCASE (sizeof(CASES) / sizeof(CASES[0]))

// 恢复出厂扫描时序
static void restoreTiming()
{
  display.epd2.expSetGateTime(0x08, 0x1a);
  display.epd2.expSetMux(GATES);
}

static void runBattery()
{
  Serial.println(F("=== PROBE START ==="));
  Serial.printf("# IL3820 probe v4: %u cases\r\n", (unsigned)NCASE);

  uint8_t bar = 0;
  for (uint16_t i = 0; i < NCASE; i++) {
    const Case &c = CASES[i];

    // 1) 波形
    lutBuild(c.phases, c.dur);
    display.epd2.expSetLut(lut, 30);

    // 2) 扫描时序
    if (c.gate >= 0 || c.dummy >= 0) {
      uint8_t g = (c.gate >= 0) ? (uint8_t)c.gate : 0x08;
      uint8_t d = (c.dummy >= 0) ? (uint8_t)c.dummy : 0x1a;
      display.epd2.expSetGateTime(g, d);
    }
    if (c.mux) display.epd2.expSetMux(c.mux);

    // 3) 画面
    if (c.drawBar) { drawCaseBar(bar); bar++; }
    else           { fbClear(0xFF); }
    display.epd2.expUploadImage(fb, FBBYTES);

    // 4) 开泵 + 计时刷新
    display.epd2.expCpOn();
    ExpTiming t;
    display.epd2.expActivateTimed(0x04, 3000, &t);

    uint32_t tframe = (gTpSum > 0) ? (t.total_us / gTpSum) : 0;
    Serial.printf("C,%u,%s,%u,%u,%d,%d,%u,%lu,%lu,%u,%u\r\n",
                  (unsigned)i, c.name, (unsigned)c.phases, (unsigned)gTpSum,
                  (int)c.gate, (int)c.dummy, (unsigned)c.mux,
                  (unsigned long)t.total_us, (unsigned long)tframe,
                  (unsigned)(t.busy_seen ? 1 : 0), (unsigned)t.edges);
    Serial.printf("# %02u %-12s ph=%u sumTP=%2u gate=%2d d=%2d mux=%3u -> %7lu us  TFRAME=%6lu us%s\r\n",
                  (unsigned)i, c.name, (unsigned)c.phases, (unsigned)gTpSum,
                  (int)c.gate, (int)c.dummy, (unsigned)c.mux,
                  (unsigned long)t.total_us, (unsigned long)tframe,
                  t.busy_seen ? "" : "  [NO BUSY]");

    // 5) 连刷抖动
    if (c.repeat > 1) {
      uint32_t mn = 0xFFFFFFFF, mx = 0, sum = 0;
      for (uint8_t r = 0; r < c.repeat; r++) {
        ExpTiming tt;
        display.epd2.expActivateTimed(0x04, 3000, &tt);
        if (tt.total_us < mn) mn = tt.total_us;
        if (tt.total_us > mx) mx = tt.total_us;
        sum += tt.total_us;
      }
      Serial.printf("#    repeat %u: avg=%lu min=%lu max=%lu  -> %.1f fps\r\n",
                    (unsigned)c.repeat, (unsigned long)(sum / c.repeat),
                    (unsigned long)mn, (unsigned long)mx,
                    1e6 / (sum / (double)c.repeat));
    }

    restoreTiming();
  }

  // 用接近出厂档的波形收尾，保证画面回到可读状态
  lutBuild(7, 3);   // sumTP=21 ≈ 出厂 20 档
  display.epd2.expSetLut(lut, 30);
  restoreTiming();
  fbClear(0xFF);
  display.epd2.expUploadImage(fb, FBBYTES);
  display.epd2.expCpOn();
  ExpTiming t;
  display.epd2.expActivateTimed(0x04, 4000, &t);

  Serial.println(F("=== PROBE END ==="));
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("##### A01 IL3820 全量波形探测 v4 #####"));

  display.init(0, true, 2, false);
  display.epd2.selectSPI(SPI, SPISettings(40000000, MSBFIRST, SPI_MODE0));
  Serial.println(F("# 标准全刷完成, SPI=40MHz"));

  runBattery();
}

void loop()
{
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'r') runBattery();
  else if (c == 'h') Serial.println(F("r=重跑"));
}
