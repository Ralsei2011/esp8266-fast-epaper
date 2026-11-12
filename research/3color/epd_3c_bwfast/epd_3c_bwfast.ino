/*
 * 三色屏纯黑白快刷 —— 第一轮可行性验证
 * ==========================================================================
 * 背景：2.9" 三色屏（SSD1680）走 GxEPD2 全程用 OTP 波形，全刷 27.2 秒。
 *       但 SSD1680 支持 0x32 写 153 字节自定义 LUT，而 0x22 的 bit4(0x10)
 *       才是"用 OTP 覆盖 LUT"的开关。三色驱动从来没写过 0x32。
 *
 * 假设：写一份黑白用的短波形 LUT，用 0x22=0xCC（不含 0x10 / 0x20）触发，
 *       三色屏就能当黑白屏快刷。
 *
 * 判据（看串口）：
 *   P2 耗时 < 2000ms  → 自定义 LUT 生效，假设成立
 *   P2 耗时 ≈ 27000ms → LUT 被忽略，要么 0x32 不被支持，要么有别的东西覆盖它
 *
 * 自动跑，不需要按键。全程约 60~120 秒。
 * 串口 115200
 * ==========================================================================
 */
#define ENABLE_GxEPD2_GFX 1

#include <SPI.h>
#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define W 128
#define H 296
#define FBB ((W / 8) * H)          // 4736

static uint8_t fbBW[FBB] __attribute__((aligned(4)));
static uint8_t fbRD[FBB] __attribute__((aligned(4)));

// ---------------------------------------------------------------------------
// 153 字节 LUT：SSD1680 黑白局刷波形
// 来源：GxEPD2_213_BN::lut_partial（同为 SSD1680，GPL-3.0）。
// 结构：前 60 字节 = 5 组 VS 表（每组 12 相），后 93 字节 = TP 表。
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
static void px(int x, int y, bool black)
{
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int idx = y * (W / 8) + (x >> 3);
  if (black) fbBW[idx] &= (uint8_t)~(0x80 >> (x & 7));
  else       fbBW[idx] |=  (uint8_t)(0x80 >> (x & 7));
}

static void fillBW(bool black) { memset(fbBW, black ? 0x00 : 0xFF, FBB); }

static void fillWhite() { fillBW(false); memset(fbRD, 0xFF, FBB); }  // 红平面全"非红"

// 测试图：黑块 / 1px 细线 / 棋盘 / 反白字块，覆盖最容易翻车的几种过渡
static void drawTest()
{
  fillWhite();
  // 左上：纯黑块 48x44
  for (int y = 20; y < 64; y++) for (int x = 10; x < 58; x++) px(x, y, true);
  // 右上：1px 竖线（黑）
  for (int x = 70; x < 118; x += 4) for (int y = 20; y < 64; y++) px(x, y, true);
  // 中间：4x4 棋盘
  for (int y = 80; y < 160; y++)
    for (int x = 10; x < 118; x++)
      if (((x / 4) + (y / 4)) % 2 == 0) px(x, y, true);
  // 下方：1px 细横线
  for (int y = 180; y < 260; y += 2) for (int x = 10; x < 118; x++) px(x, y, true);
  // 最底：文字区（用实心条代替，判读对比度）
  for (int y = 270; y < 286; y++) for (int x = 10; x < 40; x++) px(x, y, true);
}

static void upload()
{
  display.writeNative(fbBW, fbRD, 0, 0, W, H, false, false, false);
}

// 上电：0xF8 = EN_CLK + EN_CP + LOAD_TEMP + LOAD_LUT + MODE2
static void powerOn() { display.epd2.expActivate(0xF8, 800); }

static uint32_t runMode(uint8_t mode, uint32_t timeout_ms)
{
  uint32_t t = millis();
  display.epd2.expActivate(mode, timeout_ms);
  return millis() - t;
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("############ 3C BW-FAST PROBE ############");

  pinMode(PIN_BUSY, INPUT);
  Serial.printf("BUSY idle=%d  heap=%u\n", digitalRead(PIN_BUSY), ESP.getFreeHeap());

  uint32_t t0 = millis();
  display.init(115200, true, 10, false);
  Serial.printf("[init] %lu ms  heap=%u  BUSY=%d\n",
                millis() - t0, ESP.getFreeHeap(), digitalRead(PIN_BUSY));

  // ---------- P1：OTP 三色波形全刷，建立干净白底 ----------
  Serial.println("\n[P1] OTP 全刷 -> 白（基线，预期 ~27s）");
  fillWhite();
  upload();
  uint32_t t1 = runMode(0xF7, 40000);
  Serial.printf("[P1] OTP full = %lu ms\n", t1);

  // ---------- P2：写自定义 LUT + 0xCC，全白 ----------
  Serial.println("\n[P2] 写 153B 黑白 LUT + 0x22=0xCC（不含 LOAD_LUT）");
  powerOn();
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));
  fillWhite();
  upload();
  uint32_t t2 = runMode(0xCC, 40000);
  Serial.printf("[P2] LUT full = %lu ms", t2);
  if (t2 < 2000) Serial.println("   <<< 生效！自定义 LUT 起作用了");
  else if (t2 < 12000) Serial.println("   <<< 部分生效（比 OTP 快但没到毫秒级）");
  else Serial.println("   <<< 疑似未生效（仍走 OTP）");

  // ---------- P3：同一 LUT 刷测试图，看墨色 ----------
  Serial.println("\n[P3] 同 LUT 刷黑白测试图（看屏：黑块纯黑？细线糊不糊？）");
  powerOn();
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));
  drawTest();
  upload();
  uint32_t t3 = runMode(0xCC, 40000);
  Serial.printf("[P3] LUT draw = %lu ms   heap=%u\n", t3, ESP.getFreeHeap());

  display.powerOff();
  Serial.println("\n########## P1/P2/P3 完成，停住观察屏幕 ##########");
  Serial.println("屏上应该是：黑块 + 竖线 + 棋盘 + 横线，红完全不出现");
  Serial.println("若 P2 < 2s，20 秒后自动进入 P4 连刷测试");
  delay(20000);

  // ---------- P4：只有 P2 真的快才做连刷 ----------
  if (t2 >= 2000) {
    Serial.println("\n[P4] 跳过（P2 未达到毫秒级，连刷没有意义）");
    Serial.println("########## DONE ##########");
    return;
  }

  Serial.println("\n[P4] 连刷 20 帧：每帧棋盘平移 2px，测残影与稳定性");
  powerOn();
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));
  uint32_t tStart = millis();
  uint32_t sum = 0;
  for (int f = 0; f < 20; f++) {
    fillWhite();
    for (int y = 80; y < 160; y++)
      for (int x = 10; x < 118; x++)
        if ((((x + f * 2) / 4) + (y / 4)) % 2 == 0) px(x, y, true);
    upload();
    uint32_t dt = runMode(0xCC, 40000);
    sum += dt;
    if (f % 5 == 0 || f == 19) Serial.printf("  frame %2d: %lu ms\n", f, dt);
  }
  uint32_t wall = millis() - tStart;
  Serial.printf("[P4] 20 帧共 %lu ms，平均单帧 %lu ms，等效 %.1f fps\n",
                wall, sum / 20, 20000.0f / wall);

  // ---------- P5：OTP 全刷复位，留个干净画面 ----------
  Serial.println("\n[P5] OTP 全刷回白（清理现场）");
  fillWhite();
  upload();
  uint32_t t5 = runMode(0xF7, 40000);
  Serial.printf("[P5] restore = %lu ms\n", t5);

  Serial.println("\n########## DONE ##########");
  Serial.printf("汇总： OTP=%lu  LUT全白=%lu  LUT图案=%lu  连刷均值=%lu\n",
                t1, t2, t3, sum / 20);
}

void loop()
{
  delay(60000);
  Serial.printf("[idle] heap=%u\n", ESP.getFreeHeap());
}
