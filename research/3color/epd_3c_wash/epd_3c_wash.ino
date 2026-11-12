/*
 * 三色屏「洗面 + VCOM 扫描」
 *
 * 做三件事：
 *   1. 交替刷纯白/纯黑，把被错误波形搅乱的粒子状态推回干净位置
 *   2. 扫描 SSD1680 的 VCOM 寄存器 (0x2C)，找对比度最好的值
 *   3. 顺便验证放大后的字体可读性
 *
 * 用法：烧录后把板子的电源开关 OFF 等 5 秒再 ON（面板彻底断电），
 *      然后脚本会自动跑完。全程约 3 分钟。
 * 串口 115200
 */
#define ENABLE_GxEPD2_GFX 1

#include <SPI.h>
#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC   0
#define PIN_RST  2
#define PIN_BUSY 4
#define BAT_PIN  A0

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

// 待扫描的 VCOM 值。SSD1680 的 0x2C 是 8 位，具体电压档位由面板决定，
// 只能实测挑。数值越大通常 VCOM 越负。
static const uint8_t VCOM_LIST[] = {0x00, 0x40, 0x80, 0xC0};
static const uint8_t VCOM_N = sizeof(VCOM_LIST) / sizeof(VCOM_LIST[0]);

static uint8_t curVcom = 0xFF;   // 0xFF = 不覆盖，用 OTP 默认

void washScreen(uint16_t color, const char* tag)
{
  uint32_t t = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(color);
    // 角落放个标记，避免整屏纯色时看不出是否真的刷了
    display.setTextSize(2);
    display.setTextColor(color == GxEPD_WHITE ? GxEPD_BLACK : GxEPD_WHITE);
    display.setCursor(10, 10);
    display.print(tag);
  } while (display.nextPage());
  Serial.printf(">>> [WASH %s] %lu ms\n", tag, millis() - t);
}

void drawPattern(uint8_t vcom)
{
  display.fillScreen(GxEPD_WHITE);

  for (int i = 0; i < 3; i++)
    display.drawRect(i, i, display.width() - 2 * i, display.height() - 2 * i, GxEPD_BLACK);

  // 顶部大字：当前 VCOM，方便你对着屏记哪个最好
  display.setTextSize(2);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(10, 14);
  display.print("VCOM 0x");
  display.print(vcom, HEX);

  // 黑块 / 红块
  display.fillRect(10, 48, 48, 40, GxEPD_BLACK);
  display.fillRect(70, 48, 48, 40, GxEPD_RED);

  // 正文：字号 2，这是"调大一点点"的效果
  display.setCursor(10, 100);
  display.print("Hello 123");
  display.setCursor(10, 124);
  display.print("字体测试");

  // 棋盘格：看黑白是否饱和
  for (int y = 156; y < 216; y++)
    for (int x = 10; x < 118; x++)
      if (((x / 4) + (y / 4)) % 2 == 0) display.drawPixel(x, y, GxEPD_BLACK);

  // 1px 细线：看波形是否把粒子推到饱和
  for (int y = 226; y < 286; y += 4)
    display.drawFastHLine(10, y, 108, GxEPD_BLACK);

  // ★ 必须在 nextPage()/_Update_Full() 之前写 VCOM 才会被锁存
  //   注意：display 是 GxEPD2_3C 模板包装类，不透传驱动方法，要经由 epd2
  display.epd2.setVcom(vcom);
}

void refreshWithVcom(uint8_t vcom)
{
  uint32_t t = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    drawPattern(vcom);
  } while (display.nextPage());
  Serial.printf(">>> [VCOM 0x%02X] %lu ms\n", vcom, millis() - t);
}

void setup()
{
  Serial.begin(115200);
  delay(400);
  ESP.wdtEnable(10000);   // 保持看门狗开启，靠 _waitWhileBusy 里的 delay(1) 喂狗

  Serial.println();
  Serial.println("===== 3C WASH + VCOM SWEEP =====");
  Serial.printf("raw ADC(A0) = %d  (本板 A0 疑似未接分压，忽略电压读数)\n", analogRead(BAT_PIN));

  display.init(115200, true, 10, false);
  Serial.printf("init done, heap=%u\n", ESP.getFreeHeap());

  // ---- 阶段 1：洗面 ----
  Serial.println("--- washing ---");
  washScreen(GxEPD_WHITE, "WHITE 1");
  washScreen(GxEPD_BLACK, "BLACK 1");
  washScreen(GxEPD_WHITE, "WHITE 2");

  // ---- 阶段 2：VCOM 扫描 ----
  Serial.println("--- vcom sweep ---");
  for (uint8_t i = 0; i < VCOM_N; i++) {
    refreshWithVcom(VCOM_LIST[i]);
    delay(1500);
  }

  // ---- 收尾：用默认 OTP 值再刷一次，做对照 ----
  Serial.println("--- back to OTP default ---");
  curVcom = 0xFF;
  uint32_t t = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextSize(2);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(10, 20);
    display.print("OTP DEFAULT");
    display.setCursor(10, 60);
    display.print("对比一下");
    display.setCursor(10, 100);
    display.print("哪个更清楚");
  } while (display.nextPage());
  Serial.printf(">>> [OTP DEFAULT] %lu ms\n", millis() - t);

  Serial.println("========== DONE ==========");
  Serial.println("请告诉我：VCOM 0x00/0x40/0x80/0xC0 里哪一档黑块最黑、红块最正？");
  display.powerOff();
}

void loop()
{
  delay(30000);
}
