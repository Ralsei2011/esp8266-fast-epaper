/*
 * 三色屏「区域刷」边界实验
 * ==========================================================================
 * 要回答的问题（来自实测观察）：
 *   「只有半个屏幕能直刷，而且只有竖着放才能刷」——这是物理限制还是软件配置？
 *
 * 涉及的三个寄存器：
 *   0x01 GDR   门线数。只决定"扫描多少行"，扫描永远从 gate 0 开始。
 *              所以 GDR=148 必然只驱动上半屏 —— 这不是物理限制，是配置。
 *   0x44/0x45  RAM 窗口的 x/y 起止地址。
 *   0x4e/0x4f  写 RAM 的起始计数器。
 *
 * 三条待验证的假设：
 *   H1  窗口可以放在任意 y（不限于 y=0），刷新区随之移动。
 *   H2  GDR < 296 时，y 超过 GDR 的窗口刷不出来（因为根本没扫到那些门线）。
 *   H3  只改 x 范围（左半/右半）不影响能否刷新，只影响耗时以外的观感。
 *
 * 判定方法：每一步先把屏幕刷成整屏黑，再把目标区域刷成白。
 *   变了 = 该区域被扫描并驱动了；没变 = 该区域没被扫到。
 *
 * 命令（文本行，\n 结尾，2 Mbps）：
 *   K                 整屏刷黑
 *   C <x> <y> <w> <h> 该矩形刷白
 *   B <x> <y> <w> <h> 该矩形刷黑
 *   G <n>             设 GDR 门线数（0 或 296 = 整屏）
 *   S                 自动跑整套矩阵，每步停 8 秒
 *   I                 打印状态
 * ==========================================================================
 */
#define ENABLE_GxEPD2_GFX 1

#include <SPI.h>
#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define W 128
#define H 296
#define FBB ((W / 8) * H)

static uint8_t fbBW[FBB] __attribute__((aligned(4)));
static uint8_t fbRD[FBB] __attribute__((aligned(4)));

// 153 字节黑白波形表（结构同 GxEPD2_213_BN::lut_partial，GPL-3.0-or-later）
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

static uint16_t gdrLines = H;   // 当前 GDR
static char lineBuf[160];
static uint8_t lineLen = 0;

// ---------------------------------------------------------------------------
// 窗口刷：设窗口 -> 写两个平面 -> 上电装表 -> 触发
// 返回只含 0x22/0x20 那段的耗时
// ---------------------------------------------------------------------------
static uint32_t winRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint8_t bw, uint8_t rd)
{
  display.epd2.expSetRamArea(x, y, w, h);

  uint32_t n = (uint32_t)(w / 8) * h;
  memset(fbBW, bw, n);
  display.epd2.expWriteRam(0x24, fbBW, n);
  memset(fbRD, rd, n);
  display.epd2.expWriteRam(0x26, fbRD, n);

  display.epd2.expActivate(0xF8, 3000);            // 上电 + 装载 OTP 波形表
  display.epd2.expWriteLut(LUT_BW, sizeof(LUT_BW));

  uint32_t t = millis();
  display.epd2.expActivate(0xF7, 90000);
  return millis() - t;
}

static void setGdr(uint16_t n)
{
  gdrLines = (n == 0 || n > H) ? H : n;
  display.epd2.expSetGdr(gdrLines);
}

static uint32_t fullRefresh(uint8_t bw, uint8_t rd)
{
  return winRefresh(0, 0, W, H, bw, rd);
}

// ---------------------------------------------------------------------------
// 矩阵：每步先打 START 再刷新，结束后打 DONE + 耗时。
// 标签全用 ASCII，避免中文在串口日志里丢字（上一版就是这样，只剩数字）。
// 关键步骤前都先整屏刷黑，让每步的观察互不干扰。
// ---------------------------------------------------------------------------
static uint32_t runStep(const char *tag, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        uint8_t bw, uint32_t holdMs)
{
  Serial.printf("STEP %s START\r\n", tag);
  uint32_t t = winRefresh(x, y, w, h, bw, 0x00);
  Serial.printf("STEP %s DONE %lu ms\r\n", tag, (unsigned long)t);
  delay(holdMs);
  return t;
}

static void runMatrix()
{
  Serial.println(F("==== MATRIX BEGIN ===="));

  // 1) 整屏黑，建立参照
  setGdr(0);
  runStep("S1_FULL_BLACK", 0, 0, W, H, 0x00, 10000);

  // 2) 只刷下半屏 —— 窗口能否放在 y=148？
  runStep("S2_BOTTOM_WHITE_y148", 0, 148, W, 148, 0xFF, 10000);

  // 3) 重新整屏黑
  setGdr(0);
  runStep("S3_FULL_BLACK", 0, 0, W, H, 0x00, 6000);

  // 4) 只刷左半屏 —— x 方向窗口是否正常？
  runStep("S4_LEFT_WHITE_x0w64", 0, 0, 64, H, 0xFF, 10000);

  // 5) 重新整屏黑
  setGdr(0);
  runStep("S5_FULL_BLACK", 0, 0, W, H, 0x00, 6000);

  // 6) GDR=148 + 下半屏窗口：预期刷不出来（扫描只到 gate 147）
  setGdr(148);
  runStep("S6_GDR148_BOTTOM_y148", 0, 148, W, 148, 0xFF, 10000);

  // 7) GDR 仍 148，窗口改回上半屏：预期能刷
  runStep("S7_GDR148_TOP_y0", 0, 0, W, 148, 0xFF, 10000);

  // 8) 收尾全白
  setGdr(0);
  runStep("S8_FULL_WHITE", 0, 0, W, H, 0xFF, 0);

  Serial.println(F("==== MATRIX END ===="));
}

// ---------------------------------------------------------------------------
static void handle(char *s)
{
  char *cmd = strtok(s, " \t");
  if (!cmd) return;

  if (!strcmp(cmd, "K"))
  {
    uint32_t t = fullRefresh(0x00, 0x00);
    Serial.printf("OK K %lu\n", (unsigned long)t);
  }
  else if (!strcmp(cmd, "C") || !strcmp(cmd, "B"))
  {
    uint16_t x = (uint16_t)atoi(strtok(NULL, " \t"));
    uint16_t y = (uint16_t)atoi(strtok(NULL, " \t"));
    uint16_t w = (uint16_t)atoi(strtok(NULL, " \t"));
    uint16_t h = (uint16_t)atoi(strtok(NULL, " \t"));
    uint8_t v = (cmd[0] == 'C') ? 0xFF : 0x00;
    uint32_t t = winRefresh(x, y, w, h, v, 0x00);
    Serial.printf("OK %s %u %u %u %u %lu\n", cmd, x, y, w, h, (unsigned long)t);
  }
  else if (!strcmp(cmd, "G"))
  {
    setGdr((uint16_t)atoi(strtok(NULL, " \t")));
    Serial.printf("OK G %u\n", gdrLines);
  }
  else if (!strcmp(cmd, "S"))
  {
    runMatrix();
  }
  else if (!strcmp(cmd, "I"))
  {
    Serial.printf("OK I heap=%u gdr=%u\n", ESP.getFreeHeap(), gdrLines);
  }
  else
  {
    Serial.println("ERR cmd");
  }
}

// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("3C WIN TEST"));

  display.init(115200, true, 10, false);
  Serial.begin(2000000);
  delay(100);

  // 开机先整屏刷白，作为稳定基线（三色屏的 0xC8/0xF7 都要求先有一次完整刷新）
  fullRefresh(0xFF, 0x00);

  Serial.printf("READY heap=%u\r\n", ESP.getFreeHeap());
}

void loop()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (lineLen)
      {
        lineBuf[lineLen] = 0;
        handle(lineBuf);
        lineLen = 0;
      }
    }
    else if (lineLen < sizeof(lineBuf) - 1)
    {
      lineBuf[lineLen++] = c;
    }
  }
}
