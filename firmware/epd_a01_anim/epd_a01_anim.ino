/*
 * A01 (IL3820) —— 剂量阶梯 + 动画 v2
 *
 * v2 相对 v1 改了什么（对应两个实测问题）：
 *   1) 端口断开又重连、画面从头开始 -> ESP8266 复位了，不是卡死。
 *      这里在开机第一行打印复位原因，用来区分「软件崩」和「USB 掉电」：
 *        Power on / External System / Software Watchdog / Exception
 *      同时关掉 WiFi（ESP8266 最大的耗电来源），并降低连续刷新的电流冲击。
 *   2) 上一轮的残留画面没擦干净就开测 -> 新增 wash()：
 *      用出厂 FULL 波形 (sumTP=76) 走 0x08 INITIAL DISPLAY 做真正的 GC 全清，
 *      每一段测试之前都先擦一遍。
 *
 * 时基（前两轮实测）：
 *   TFRAME = mux × 30us      mux=296 -> 9.18ms
 *   一帧   = sumTP × TFRAME
 *
 * 串口日志：
 *   R,<复位原因>,<空闲堆>             开机自报
 *   W,<次数>,<us>                     擦屏
 *   D,<脉冲>,<us>                     剂量阶梯
 *   A,<sumTP>,<帧>,<up_us>,<scan_us>,<total_us>
 *   S,<sumTP>,<帧>,<avg_us>,<fps>,<min>,<max>
 */

#include <GxEPD2_BW.h>
#include <ESP8266WiFi.h>

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

// 出厂局刷：7 个驱动相
static const uint8_t PH_PART[7] = { 0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08 };

// 出厂全刷（GxEPD2 LUTDefault_full 的 30 字节数据部分），sumTP = 76
static const uint8_t LUT_FULL[30] = {
  0x50, 0xAA, 0x55, 0xAA, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ---------------------------------------------------------------- 波形

static void lutFast(uint8_t phases, uint8_t dur)
{
  memset(lut, 0, 30);
  for (uint8_t p = 0; p < phases && p < 7; p++) lut[p] = PH_PART[p];
  for (uint8_t p = 0; p < 20; p++) {
    uint8_t tp = (p < phases) ? dur : 0;
    uint8_t k = p >> 1;
    if (p & 1) lut[20 + k] |= (uint8_t)((tp & 0x0F) << 4);
    else       lut[20 + k] |= (uint8_t)(tp & 0x0F);
  }
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

static void upload() { display.epd2.expUploadImage(fb, FBBYTES); }

static ExpTiming timedRefresh(uint8_t mode, uint32_t to)
{
  ExpTiming t;
  display.epd2.expActivateTimed(mode, to, &t);
  return t;
}

// ---------------------------------------------------------------- 擦屏

// 擦屏：用出厂 FULL 波形（sumTP=76 = 1572ms）走 0x04 连续刷白
// 不用 0x08(INITIAL DISPLAY)：实测它会超时 6s，而且会把后续时基搞乱
// （之后 sumTP=4 的帧从 36.7ms 变成 56.2ms），所以这里绕开它。
static void wash(uint8_t times)
{
  display.epd2.expSetLut(LUT_FULL, 30);
  display.epd2.expSetGateTime(0x08, 0x1a);
  display.epd2.expSetMux(GATES);
  fbClear(0xFF);
  upload();
  display.epd2.expCpOn();
  for (uint8_t i = 0; i < times; i++) {
    ExpTiming t = timedRefresh(0x04, 6000);
    Serial.printf("W,%u,%lu,%u\r\n", (unsigned)(i + 1),
                  (unsigned long)t.total_us, (unsigned)t.edges);
  }
  Serial.println(F("# 已擦屏(全刷波形刷白)"));
}

// ---------------------------------------------------------------- A 剂量阶梯
static void doseLadder()
{
  Serial.println(F(">>> A 剂量阶梯：黑区逐次变深，记第几次黑透"));
  wash(2);

  // 目标：中间大黑区 + 底部 20 格刻度
  fbClear(0xFF);
  fbRect(40, 250, 20, 108, 0);
  for (uint8_t k = 0; k < 20; k++) fbRect(280, 290, 4 + k * 6, 8 + k * 6, 0);
  upload();
  display.epd2.expCpOn();
  ExpTiming t0 = timedRefresh(0x04, 4000);   // 用当前(全刷)波形先把目标画上去
  Serial.printf("# 目标已绘制 %lu us\r\n", (unsigned long)t0.total_us);

  // 切 sumTP=2 的短脉冲（2 个相位各 1 帧 = 18.4ms）
  lutFast(2, 1);
  display.epd2.expSetLut(lut, 30);
  display.epd2.expSetGateTime(0x00, 0x00);
  display.epd2.expSetMux(GATES);

  Serial.println(F("# 阶梯：sumTP=2 脉冲 x16"));
  for (uint8_t k = 1; k <= 16; k++) {
    ExpTiming t = timedRefresh(0x04, 1000);
    Serial.printf("D,%u,%lu\r\n", (unsigned)k, (unsigned long)t.total_us);
    Serial.printf("#   脉冲 %2u  %5lu us\r\n", (unsigned)k, (unsigned long)t.total_us);
    delay(450);
  }
  Serial.println(F(">>> A 结束"));
}

// ---------------------------------------------------------------- B 动画
static void playAnim(uint8_t phases, uint8_t dur, uint8_t frames, uint32_t to)
{
  uint8_t sumTp = (uint8_t)(phases * dur);
  Serial.printf(">>> B 动画 sumTP=%u\r\n", (unsigned)sumTp);
  wash(1);

  lutFast(phases, dur);
  display.epd2.expSetLut(lut, 30);
  display.epd2.expSetGateTime(0x00, 0x00);
  display.epd2.expSetMux(GATES);
  display.epd2.expCpOn();

  uint32_t sum = 0, mx = 0, mn = 0xFFFFFFFF;
  for (uint8_t f = 0; f < frames; f++) {
    int16_t cx = 14 + (int16_t)((SOURCES - 28) * (0.5 + 0.5 * sin(f * 0.314)));
    int16_t cy = 148 + (int16_t)(130 * sin(f * 0.157));
    fbClear(0xFF);
    fbDisc(cx, cy, 26, 0);
    int16_t gx = (int16_t)((int32_t)GATES * f / frames);
    fbRect(gx, gx + 3, 0, SOURCES - 1, 0);

    uint32_t a0 = micros();
    upload();
    uint32_t a1 = micros();
    ExpTiming t = timedRefresh(0x04, to);
    uint32_t a2 = micros();

    uint32_t total = a2 - a0;
    sum += total;
    if (total > mx) mx = total;
    if (total < mn) mn = total;

    Serial.printf("A,%u,%u,%lu,%lu,%lu\r\n", (unsigned)sumTp, (unsigned)f,
                  (unsigned long)(a1 - a0), (unsigned long)t.total_us,
                  (unsigned long)total);
    if ((f + 1) % 8 == 0) {
      Serial.printf("#   f%02u up=%lu scan=%lu edges=%u total=%lu  heap=%u\r\n", (unsigned)f,
                    (unsigned long)(a1 - a0), (unsigned long)t.total_us,
                    (unsigned)t.edges, (unsigned long)total, ESP.getFreeHeap());
      yield();
    }
    // 给电源一个呼吸间隔，压低连续刷新对 USB 的冲击
    delay(2);
  }
  Serial.printf("S,%u,%u,%lu,%.1f,%lu,%lu\r\n",
                (unsigned)sumTp, (unsigned)frames,
                (unsigned long)(sum / frames), 1e6 / (sum / (double)frames),
                (unsigned long)mn, (unsigned long)mx);
  Serial.printf("#   汇总 sumTP=%u avg=%lu us  %.1f fps  (min %lu / max %lu)\r\n",
                (unsigned)sumTp, (unsigned long)(sum / frames),
                1e6 / (sum / (double)frames), (unsigned long)mn, (unsigned long)mx);
}

// ---------------------------------------------------------------- C 收尾
static void finishNormal()
{
  wash(2);
  display.epd2.expSetLut(LUT_FULL, 30);
  display.epd2.expSetGateTime(0x08, 0x1a);
  display.epd2.expSetMux(GATES);
  fbClear(0xFF);
  fbRect(40, 250, 20, 108, 0);
  fbRect(12, 46, 56, 120, 0);
  fbRect(60, 70, 10, 40, 0);
  upload();
  display.epd2.expCpOn();
  ExpTiming tf = timedRefresh(0x04, 6000);
  Serial.printf("# 收尾刷新 %lu us\r\n", (unsigned long)tf.total_us);
  Serial.println(F("# 已恢复正常全刷画面"));
}

static void runAll()
{
  Serial.println(F("=== PROBE START ==="));
  wash(2);
  doseLadder();
  playAnim(4, 1, 32, 1500);   // sumTP=4  -> 36.7ms/帧
  playAnim(2, 1, 32, 1500);   // sumTP=2  -> 18.4ms/帧
  playAnim(6, 1, 24, 1500);   // sumTP=6  -> 55.1ms/帧
  finishNormal();
  Serial.println(F("=== PROBE END ==="));
}

void setup()
{
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("##### A01 IL3820 剂量阶梯 + 动画 v2 #####"));
  Serial.printf("R,%s,%u\r\n", ESP.getResetReason().c_str(), ESP.getFreeHeap());
  Serial.printf("# 复位原因：%s   空闲堆 %u\r\n",
                ESP.getResetReason().c_str(), ESP.getFreeHeap());

  display.init(0, true, 2, false);
  display.epd2.selectSPI(SPI, SPISettings(40000000, MSBFIRST, SPI_MODE0));
  Serial.println(F("# 标准全刷完成, SPI=40MHz, WiFi=OFF"));

  runAll();
}

void loop()
{
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'r') runAll();
  else if (c == 'w') wash(2);
  else if (c == 'h') Serial.println(F("r=重跑 w=擦屏"));
}
