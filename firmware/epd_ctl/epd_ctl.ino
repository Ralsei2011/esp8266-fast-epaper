/*
 * epd_ctl —— A01 (IL3820 / SSD1608, 2.9" 128x296 黑白) 通用脚本驱动固件
 *
 * 为什么做它：之前每改一个测试参数都要重新编译 60 秒，太慢。
 * 现在这个固件只烧一次，之后改测试完全靠串口文本命令 / 脚本文件。
 *
 * 命令（一行一条，\n 结尾，# 开头为注释）：
 *   ver                     打印版本
 *   clr <0|1>               清缓冲：1=白 0=黑
 *   bar <idx> [w]           画第 idx 根竖条（x = 2 + idx*6）
 *   rect <g0> <g1> <s0> <s1> <v>
 *   disc <cx> <cy> <r> <v>  实心圆
 *   up [fast|slow]          上传缓冲到显存，回 U,<us>,<mode>
 *   lut wave <ph> <dur>     用时序可调的快速波形（出厂 7 个驱动相）
 *   lut full                出厂全刷波形（sumTP=76）
 *   lut part                出厂局刷波形（sumTP=20）
 *   lut hex <60位hex>       直接写 30 字节波形
 *   gate <g> <d>            0x3B TGate / 0x3A DummyLine
 *   mux <n>                 0x01 有效门线数
 *   temp <摄氏度>           0x1A 写温度寄存器
 *   cp | clk | off          0x22 = C0 / 80 / 03
 *   mode <hex> [to_ms]      0x22 触发并计时 → T,<mode>,<total>,<high>,<edges>,<busy>,<timeout>
 *   scan [to_ms]            等价 mode 04
 *   anim <ph> <dur> <frames> [gap_ms]  播放弹跳圆动画
 *   wash [n]                用库的 clearScreen 刷白 n 次（默认 2）
 *   heap                    打印空闲堆
 *
 * 时基（实测）：
 *   TFRAME = mux × 30us        mux=296 → 9.18ms
 *   一帧   = sumTP × TFRAME
 */

#include <GxEPD2_BW.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h>

// 内部 1.1V 基准测 VCC（不需要 A0 接任何东西）
ADC_MODE(ADC_VCC);

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
#define LINE_MAX 160

// 注意 aligned(4)：ESP8266 的 SPI.writeBytes() 内部把指针当 uint32_t* 用，
// 缓冲区不是 4 字节对齐就会抛 Exception 9（LoadStoreAlignmentCause）。
static uint8_t fb[FBBYTES] __attribute__((aligned(4)));
static uint8_t lut[30];

static const uint8_t PH_PART[7] = { 0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08 };

static const uint8_t LUT_FULL[30] = {
  0x50, 0xAA, 0x55, 0xAA, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t LUT_PART[30] = {
  0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x13, 0x14, 0x44, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t gPhases = 0, gDur = 0;
static bool gFastUpload = true;

// ---------------------------------------------------------------- 波形

static void lutWave(uint8_t phases, uint8_t dur)
{
  memset(lut, 0, 30);
  for (uint8_t p = 0; p < phases && p < 7; p++) lut[p] = PH_PART[p];
  for (uint8_t p = 0; p < 20; p++) {
    uint8_t tp = (p < phases) ? dur : 0;
    uint8_t k = p >> 1;
    if (p & 1) lut[20 + k] |= (uint8_t)((tp & 0x0F) << 4);
    else       lut[20 + k] |= (uint8_t)(tp & 0x0F);
  }
  gPhases = phases; gDur = dur;
  display.epd2.expSetLut(lut, 30);
}

// ---------------------------------------------------------------- 画面

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
      if (v) row[x >> 3] |= m; else row[x >> 3] &= (uint8_t)~m;
    }
  }
}

static void fbDisc(int16_t cx, int16_t cy, int16_t r, uint8_t v)
{
  for (int16_t dy = -r; dy <= r; dy++) {
    int16_t y = cy + dy;
    if (y < 0 || y >= GATES) continue;
    int16_t span = (int16_t)sqrt((float)(r * r - dy * dy));
    fbRect(y, y, cx - span, cx + span, v);
  }
}

static uint32_t doUpload(bool fast)
{
  uint32_t t0 = micros();
  if (fast) display.epd2.expUploadImageFast(fb, FBBYTES);
  else      display.epd2.expUploadImage(fb, FBBYTES);
  return micros() - t0;
}

static void doMode(uint8_t mode, uint32_t to)
{
  ExpTiming t;
  display.epd2.expActivateTimed(mode, to, &t);
  Serial.printf("T,%02X,%lu,%lu,%u,%u,%u\r\n", mode,
                (unsigned long)t.total_us, (unsigned long)t.high_us,
                (unsigned)t.edges, (unsigned)(t.busy_seen ? 1 : 0),
                (unsigned)(t.timeout ? 1 : 0));
}

// ---------------------------------------------------------------- 命令实现

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------- 黑盒
//
// 目的：区分「真掉电」和「被 RST 脚 / 看门狗复位」。
//
//   RTC 用户内存：软件复位、WDT 复位、RST 拉低复位 → 内容保留
//                 真正的电源掉电（VCC 跌破阈值）    → 内容丢失
//
// 所以只要看 RTC magic 是否还在，就能判断上一次到底是哪种复位。
// 同时把每次启动写进 EEPROM（flash），掉电也不会丢历史。

#define BB_MAGIC 0x45504442UL // "EPDB"

struct BlackBox {
  uint32_t magic;
  uint32_t boots;       // 累计启动次数
  uint32_t lastFrame;   // 上一次 anim 跑到第几帧
  uint32_t lastMs;      // 上一次 anim 运行了多久
  uint32_t lastReason;  // 上一次启动时读到的复位原因
};

static BlackBox bb;
static BlackBox bbPrev;

static void bbSave()
{
  bb.magic = BB_MAGIC;
  EEPROM.put(0, bb);
  EEPROM.commit();
}

static void bbUpdate(uint32_t frame, uint32_t ms)
{
  bb.lastFrame = frame;
  bb.lastMs = ms;
  uint32_t rtc[3] = { BB_MAGIC, frame, ms };
  ESP.rtcUserMemoryWrite(0, rtc, sizeof(rtc));
}

static void bbBoot()
{
  EEPROM.begin(sizeof(BlackBox) + 8);
  EEPROM.get(0, bbPrev);
  if (bbPrev.magic != BB_MAGIC) { memset(&bbPrev, 0, sizeof(bbPrev)); bbPrev.boots = 0; }

  uint32_t rtc[3] = { 0, 0, 0 };
  ESP.rtcUserMemoryRead(0, rtc, sizeof(rtc));
  bool rtcAlive = (rtc[0] == BB_MAGIC);

  bb = bbPrev;
  bb.boots++;

  struct rst_info *ri = ESP.getResetInfoPtr();
  Serial.printf("# ===== BOOT #%lu  reason=%u (%s) =====\r\n",
                (unsigned long)bb.boots, (unsigned)ri->reason,
                ESP.getResetReason().c_str());
  Serial.printf("# info: %s\r\n", ESP.getResetInfo().c_str());
  Serial.printf("# RTC %s  lastFrame=%lu lastMs=%lu\r\n",
                rtcAlive ? "存活(非掉电)" : "已丢失(真掉电)",
                (unsigned long)(rtcAlive ? rtc[1] : 0),
                (unsigned long)(rtcAlive ? rtc[2] : 0));
  Serial.printf("# flash: prevBoots=%lu prevLastFrame=%lu prevLastMs=%lu\r\n",
                (unsigned long)bbPrev.boots, (unsigned long)bbPrev.lastFrame,
                (unsigned long)bbPrev.lastMs);
  Serial.printf("# BOOTINFO,%u,%s,%u,%lu,%lu\r\n",
                (unsigned)ri->reason, rtcAlive ? "alive" : "lost",
                (unsigned)bb.boots,
                (unsigned long)(rtcAlive ? rtc[1] : 0),
                (unsigned long)(rtcAlive ? rtc[2] : 0));
  Serial.printf("# VCC=%umV\r\n", (unsigned)ESP.getVcc());

  bb.lastReason = ri->reason;
  bbSave();
  bbUpdate(0, 0);
  ESP.rtcUserMemoryWrite(0, rtc, sizeof(rtc)); // 立即把 magic 写回去
}

static void cmdAnim(int phases, int dur, int frames, int gap, int fstart, int draw, int both, int sw)
{
  if (sw < 0) sw = 0;
  if (sw > SOURCES - 1) sw = SOURCES - 1;
  if (phases > 0) lutWave((uint8_t)phases, (uint8_t)dur);
  display.epd2.expSetGateTime(0x00, 0x00);
  display.epd2.expSetMux(GATES);
  display.epd2.expCpOn();

  uint32_t sum = 0, mx = 0, mn = 0xFFFFFFFF;
  uint32_t t_start = millis();
  uint16_t vmin = 9999, vmax = 0;
  for (int i = 0; i < frames; i++) {
    int f = fstart + i;

    fbClear(0xFF);

    if (draw & 1) {
      // ① 水平扫描条：每帧右移 3px，匀速平移最容易看出「卡不卡」
      int16_t gx = (int16_t)(((int32_t)(f % 100) * 3) % (GATES + 10)) - 5;
      fbRect(gx, gx + 5, 0, sw, 0);
    }
    if (draw & 4) {
      // ④ 同样的横条，但只画左半幅（64 个 source）
      int16_t gx = (int16_t)(((int32_t)(f % 100) * 3) % (GATES + 10)) - 5;
      fbRect(gx, gx + 5, 0, (SOURCES / 2) - 1, 0);
    }
    if (draw & 8) {
      // ⑧ 同样是全宽横条，但只有 1 行高
      int16_t gx = (int16_t)(((int32_t)(f % 100) * 3) % (GATES + 10)) - 5;
      fbRect(gx, gx, 0, SOURCES - 1, 0);
    }
    if (draw & 16) {
      // ⑯ 竖向条：6 个 source 宽、贯穿全部 gate，横向移动
      int16_t sx = (int16_t)(((int32_t)(f % 100) * 2) % (SOURCES + 8)) - 4;
      fbRect(0, GATES - 1, sx, sx + 5, 0);
    }
    if (draw & 2) {
      // ② 上下弹跳的圆：周期约 40 帧（≈1.5s），位移小、轨迹连续
      int16_t cx = 22 + (int16_t)(84.0 * (0.5 + 0.5 * sin(f * 0.0628)));
      int16_t cy = 74 + (int16_t)(46.0 * sin(f * 0.157));
      fbDisc(cx, cy, 18, 0);
    }

    uint32_t up = doUpload(gFastUpload);
    ExpTiming t;
    display.epd2.expActivateTimed(0x04, 2000, &t);
    if (both) {
      // 刷新之后把本帧写进 0x26，作为下一帧的“旧帧”。
      // 不这么做的话 0x26 永远是洗白那张，旧图案擦不掉、不匹配像素逐帧累积。
      display.epd2.expUploadOldFast(fb, FBBYTES);
    }
    uint32_t total = up + t.total_us;
    sum += total;
    if (total > mx) mx = total;
    if (total < mn) mn = total;

    // 串口每行约 2.6ms（115200）。诊断阶段每帧都打，带 VCC 采样。
    uint16_t v = ESP.getVcc();
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
    Serial.printf("A,%d,%lu,%lu,%lu,%u\r\n", f, (unsigned long)up,
                  (unsigned long)t.total_us, (unsigned long)total, (unsigned)v);
    if (gap > 0) delay(gap);
    if ((i & 7) == 7) { yield(); }
    bbUpdate((uint32_t)f, millis() - t_start);
  }
  bbSave();
  Serial.printf("S,%d,%d,%lu,%.1f,%lu,%lu\r\n", phases * dur, frames,
                (unsigned long)(sum / frames), 1e6 / (sum / (double)frames),
                (unsigned long)mn, (unsigned long)mx);
  Serial.printf("V,%u,%u\r\n", (unsigned)vmin, (unsigned)vmax);
}

// blink <n> [gap] —— 全屏黑白交替，每帧翻转 296x128 全部像素（最大电荷负载）
static void cmdBlink(int n, int gap)
{
  lutWave(4, 1);
  display.epd2.expSetGateTime(0x00, 0x00);
  display.epd2.expSetMux(GATES);
  display.epd2.expCpOn();

  uint32_t t0 = millis();
  for (int i = 0; i < n; i++) {
    fbClear((i & 1) ? 0xFF : 0x00);
    doUpload(true);
    ExpTiming t;
    display.epd2.expActivateTimed(0x04, 2000, &t);
    if ((i % 5) == 0) Serial.printf("A,%d,0,%lu,%lu\r\n", i,
                                    (unsigned long)t.total_us, (unsigned long)t.total_us);
    if (gap) delay(gap);
    bbUpdate((uint32_t)i, millis() - t0);
  }
  bbSave();
  Serial.printf("S,blink,%d\r\n", n);
}

static void runLine(char *s)
{
  // 取第一个词
  char *cmd = strtok(s, " \t");
  if (!cmd) return;

  if (!strcmp(cmd, "ver")) {
    Serial.println(F("OK ver epd_ctl-1  IL3820 128x296"));
  }
  else if (!strcmp(cmd, "heap")) {
    Serial.printf("OK heap %u\r\n", ESP.getFreeHeap());
  }
  else if (!strcmp(cmd, "clr")) {
    char *a = strtok(NULL, " \t");
    uint8_t v = (a && atoi(a) == 0) ? 0x00 : 0xFF;
    fbClear(v);
    Serial.printf("OK clr %u\r\n", v ? 1 : 0);
  }
  else if (!strcmp(cmd, "bar")) {
    char *a = strtok(NULL, " \t");
    char *b = strtok(NULL, " \t");
    int idx = a ? atoi(a) : 0;
    int w = b ? atoi(b) : 4;
    fbClear(0xFF);
    int x = 2 + idx * 6;
    if (x > SOURCES - 2) x = SOURCES - 2;
    fbRect(0, GATES - 1, x, x + w - 1, 0);
    Serial.printf("OK bar %d x=%d w=%d\r\n", idx, x, w);
  }
  else if (!strcmp(cmd, "rect")) {
    int v[5] = {0, 0, 0, 0, 1};
    for (int i = 0; i < 5; i++) {
      char *a = strtok(NULL, " \t");
      if (!a) break;
      v[i] = atoi(a);
    }
    fbRect(v[0], v[1], v[2], v[3], (uint8_t)v[4]);
    Serial.printf("OK rect %d,%d,%d,%d,%d\r\n", v[0], v[1], v[2], v[3], v[4]);
  }
  else if (!strcmp(cmd, "disc")) {
    int v[4] = {64, 148, 26, 0};
    for (int i = 0; i < 4; i++) {
      char *a = strtok(NULL, " \t");
      if (!a) break;
      v[i] = atoi(a);
    }
    fbDisc(v[0], v[1], v[2], (uint8_t)v[3]);
    Serial.printf("OK disc %d,%d,%d,%d\r\n", v[0], v[1], v[2], v[3]);
  }
  else if (!strcmp(cmd, "up")) {
    char *a = strtok(NULL, " \t");
    bool fast = gFastUpload;
    bool both = false;
    if (a && !strcmp(a, "slow")) fast = false;
    if (a && !strcmp(a, "fast")) fast = true;
    if (a && !strcmp(a, "both")) { fast = true; both = true; }
    uint32_t us = doUpload(fast);
    if (both) {
      // 全刷（0x08 / FULL 波形）时 IC 会拿 0x24 与 0x26 做比较，
      // 只写 0x24 而 0x26 是脏的 → 全刷结果错乱。这里把旧帧也写成同一幅图。
      uint32_t t0 = micros();
      display.epd2.expUploadOldFast(fb, FBBYTES);
      us += micros() - t0;
    }
    Serial.printf("U,%lu,%s\r\n", (unsigned long)us, both ? "both" : (fast ? "fast" : "slow"));
  }
  else if (!strcmp(cmd, "lut")) {
    char *a = strtok(NULL, " \t");
    if (!a) { Serial.println(F("ERR lut")); return; }
    if (!strcmp(a, "wave")) {
      char *p = strtok(NULL, " \t");
      char *d = strtok(NULL, " \t");
      lutWave((uint8_t)(p ? atoi(p) : 4), (uint8_t)(d ? atoi(d) : 1));
      Serial.printf("OK lut wave ph=%u dur=%u sumTP=%u\r\n",
                    gPhases, gDur, (unsigned)(gPhases * gDur));
    }
    else if (!strcmp(a, "full")) {
      display.epd2.expSetLut(LUT_FULL, 30);
      Serial.println(F("OK lut full sumTP=76"));
    }
    else if (!strcmp(a, "part")) {
      display.epd2.expSetLut(LUT_PART, 30);
      Serial.println(F("OK lut part sumTP=20"));
    }
    else if (!strcmp(a, "vs")) {
      // lut vs <vs_hex> <tp> [<vs_hex> <tp> ...]  最多 7 相
      memset(lut, 0, 30);
      uint8_t n = 0;
      for (;;) {
        char *h = strtok(NULL, " \t");
        char *t = strtok(NULL, " \t");
        if (!h || !t || n >= 7) break;
        int hi = hexval(h[0]), lo = hexval(h[1]);
        if (hi < 0 || lo < 0) break;
        lut[n] = (uint8_t)(hi * 16 + lo);
        uint8_t tp = (uint8_t)atoi(t);
        uint8_t k = 20 + (n >> 1);
        if (n & 1) lut[k] |= (uint8_t)((tp & 0x0F) << 4);
        else       lut[k] |= (uint8_t)(tp & 0x0F);
        n++;
      }
      if (!n) { Serial.println(F("ERR lut vs")); return; }
      display.epd2.expSetLut(lut, 30);
      uint8_t ssum = 0;
      for (int q = 0; q < 10; q++) ssum += (lut[20 + q] & 0x0F) + (lut[20 + q] >> 4);
      Serial.printf("OK lut vs n=%u sumTP=%u\r\n", n, (unsigned)ssum);
    }
    else if (!strcmp(a, "hex")) {
      char *h = strtok(NULL, " \t");
      if (!h || strlen(h) < 60) { Serial.println(F("ERR lut hex 需 60 位十六进制")); return; }
      for (int i = 0; i < 30; i++) {
        int hi = hexval(h[i * 2]), lo = hexval(h[i * 2 + 1]);
        if (hi < 0 || lo < 0) { Serial.println(F("ERR lut hex 非法字符")); return; }
        lut[i] = (uint8_t)(hi * 16 + lo);
      }
      display.epd2.expSetLut(lut, 30);
      uint8_t s = 0;
      for (int k = 0; k < 10; k++) s += (lut[20 + k] & 0x0F) + (lut[20 + k] >> 4);
      Serial.printf("OK lut hex sumTP=%u\r\n", (unsigned)s);
    }
    else Serial.println(F("ERR lut"));
  }
  else if (!strcmp(cmd, "gate")) {
    char *a = strtok(NULL, " \t");
    char *b = strtok(NULL, " \t");
    uint8_t g = (uint8_t)(a ? atoi(a) : 0);
    uint8_t d = (uint8_t)(b ? atoi(b) : 0);
    display.epd2.expSetGateTime(g, d);
    Serial.printf("OK gate %u %u\r\n", g, d);
  }
  else if (!strcmp(cmd, "mux")) {
    char *a = strtok(NULL, " \t");
    uint16_t n = (uint16_t)(a ? atoi(a) : GATES);
    display.epd2.expSetMux(n);
    Serial.printf("OK mux %u\r\n", n);
  }
  else if (!strcmp(cmd, "temp")) {
    char *a = strtok(NULL, " \t");
    float c = a ? atof(a) : 25.0f;
    uint16_t raw = (uint16_t)(c * 16.0f + 0.5f);
    display.epd2.expWriteTemp(raw);
    Serial.printf("OK temp %.2fC raw=%u\r\n", c, raw);
  }
  else if (!strcmp(cmd, "cp"))  { display.epd2.expCpOn();  Serial.println(F("OK cp")); }
  else if (!strcmp(cmd, "clk")) { display.epd2.expClockOn(); Serial.println(F("OK clk")); }
  else if (!strcmp(cmd, "off")) { display.epd2.expAllOff();  Serial.println(F("OK off")); }
  else if (!strcmp(cmd, "mode")) {
    char *a = strtok(NULL, " \t");
    char *b = strtok(NULL, " \t");
    uint8_t m = (uint8_t)(a ? strtol(a, NULL, 16) : 0x04);
    uint32_t to = (uint32_t)(b ? atoi(b) : 2000);
    doMode(m, to);
  }
  else if (!strcmp(cmd, "scan")) {
    char *a = strtok(NULL, " \t");
    doMode(0x04, (uint32_t)(a ? atoi(a) : 2000));
  }
  else if (!strcmp(cmd, "anim")) {
    int v[8] = {4, 1, 24, 0, 0, 3, 0, SOURCES - 1};
    for (int i = 0; i < 8; i++) {
      char *a = strtok(NULL, " \t");
      if (!a) break;
      v[i] = atoi(a);
    }
    cmdAnim(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
  }
  else if (!strcmp(cmd, "wash")) {
    char *a = strtok(NULL, " \t");
    int n = a ? atoi(a) : 2;
    // 先强制休眠再唤醒：把 0x22 / LUT / 0x3A / 0x3B 全部拉回上电默认，
    // 否则前面实验留下的自定义波形会让 clearScreen 刷不出纯白。
    display.hibernate();
    for (int i = 0; i < n; i++) display.clearScreen(0xFF);
    Serial.printf("OK wash %d\r\n", n);
  }
  else if (!strcmp(cmd, "vcc")) {
    Serial.printf("VCC,%u\r\n", (unsigned)ESP.getVcc());
  }
  else if (!strcmp(cmd, "blink")) {
    char *a = strtok(NULL, " \t");
    char *b = strtok(NULL, " \t");
    cmdBlink(a ? atoi(a) : 20, b ? atoi(b) : 0);
  }
  else if (!strcmp(cmd, "bb")) {
    uint32_t rtc[3] = { 0, 0, 0 };
    ESP.rtcUserMemoryRead(0, rtc, sizeof(rtc));
    Serial.printf("BB,%lu,%lu,%lu,%lu,%s,%lu,%lu\r\n",
                  (unsigned long)bb.boots, (unsigned long)bb.lastFrame,
                  (unsigned long)bb.lastMs, (unsigned long)bb.lastReason,
                  (rtc[0] == BB_MAGIC) ? "alive" : "lost",
                  (unsigned long)rtc[1], (unsigned long)rtc[2]);
  }
  else {
    Serial.printf("ERR unknown: %s\r\n", cmd);
  }
}

// ---------------------------------------------------------------- 主循环

static char lineBuf[LINE_MAX];
static uint8_t lineLen = 0;

void setup()
{
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  Serial.begin(115200);
  delay(150);

  Serial.println();
  Serial.println(F("##### epd_ctl ready #####"));
  bbBoot();
  Serial.printf("# reset=%s heap=%u\r\n", ESP.getResetReason().c_str(), ESP.getFreeHeap());

  display.init(0, true, 2, false);
  display.epd2.selectSPI(SPI, SPISettings(40000000, MSBFIRST, SPI_MODE0));
  Serial.println(F("# init done, SPI=40MHz"));
  Serial.println(F("READY"));
}

void loop()
{
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      if (lineLen && lineBuf[0] != '#') runLine(lineBuf);
      lineLen = 0;
      Serial.println(F("DONE"));
    } else if (lineLen < LINE_MAX - 1) {
      lineBuf[lineLen++] = c;
    }
  }
  yield();
}
