/*
 * 三色刷新基准测试（单一驱动，排除一切干扰）
 *
 * 目的：搞清「残影/发淡」是电压不够，还是波形根本没走完。
 * 判据：GxEPD2 的 _Update_Full 会打印耗时（微秒）。
 *       - 若接近 27000000 → 打满 full_refresh_time 超时，面板没刷完
 *       - 若接近 19000000 → 真实完成
 *
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

float readVbat()
{
  uint32_t s = 0;
  for (int i = 0; i < 30; i++) s += analogRead(BAT_PIN);
  return (s / 30.0f) * 0.0009765625f * 5.607f;
}

void drawPattern(const char* tag)
{
  display.fillScreen(GxEPD_WHITE);

  // 外框：4px 粗，用来判断边缘是否被完整驱动
  for (int i = 0; i < 3; i++)
    display.drawRect(i, i, display.width() - 2 * i, display.height() - 2 * i, GxEPD_BLACK);

  display.setTextColor(GxEPD_BLACK);
  display.setCursor(10, 26);
  display.print(tag);

  // 黑块 / 红块：三色各占一块，判断两个粒子都动没动
  display.fillRect(10, 60, 48, 44, GxEPD_BLACK);
  display.fillRect(70, 60, 48, 44, GxEPD_RED);

  // 灰度阶梯（只有黑白两色，画棋盘格看对比度是否饱和）
  for (int y = 120; y < 180; y++)
    for (int x = 10; x < 118; x++)
      if (((x / 4) + (y / 4)) % 2 == 0) display.drawPixel(x, y, GxEPD_BLACK);

  // 1px 细线：判断波形是否推到饱和
  for (int y = 190; y < 250; y += 4)
    display.drawFastHLine(10, y, 108, GxEPD_BLACK);

  display.setCursor(10, 270);
  display.print("1px lines / checker");
}

void fullRefresh(const char* tag)
{
  uint32_t t = millis();
  display.setFullWindow();
  display.firstPage();
  do { drawPattern(tag); } while (display.nextPage());
  uint32_t dt = millis() - t;
  Serial.printf(">>> [%s] %lu ms  (timeout=%d)  heap=%u  vbat=%.2fV\n",
                tag, dt, (int)GxEPD2_290_C90c::full_refresh_time,
                ESP.getFreeHeap(), readVbat());
}

void setup()
{
  Serial.begin(115200);
  delay(400);

  // ★ 关键：不要 ESP.wdtDisable()。
  // core 3.x 里它只关软件看门狗，硬件看门狗照样在 8 秒后硬复位，
  // 而 delay() 不再喂狗 → 刷新到一半被复位 → 面板波形中断 → 画面发淡。
  // 保持看门狗开启，GxEPD2 的 _waitWhileBusy 里用 delay(1)，会自然喂狗。
  ESP.wdtEnable(10000);

  Serial.println();
  Serial.println("===== 3C REFRESH BASELINE =====");
  Serial.printf("raw ADC(A0) = %d   vbat = %.3f V\n", analogRead(BAT_PIN), readVbat());
  if (readVbat() < 1.0f)
    Serial.println("!! A0 读到接近 0，这块板可能没接电池分压，vbat 数据不可信，忽略即可");

  pinMode(PIN_BUSY, INPUT);
  Serial.printf("BUSY idle level  : %d   (C90c 期望 busy=HIGH, 所以空闲应为 0)\n",
                digitalRead(PIN_BUSY));

  Serial.println("init(115200, initial=true, reset=10ms) ...");
  uint32_t t0 = millis();
  display.init(115200, true, 10, false);
  Serial.printf("init done in %lu ms, heap=%u, BUSY=%d\n",
                millis() - t0, ESP.getFreeHeap(), digitalRead(PIN_BUSY));

  for (int r = 1; r <= 2; r++) {
    char tag[16];
    snprintf(tag, sizeof(tag), "ROUND %d", r);
    fullRefresh(tag);
    ESP.wdtFeed();
    delay(2000);
  }

  Serial.println("========== DONE ==========");
  Serial.printf("final heap = %u, raw ADC = %d\n", ESP.getFreeHeap(), analogRead(BAT_PIN));
  Serial.println("看屏幕：黑块纯黑？红块正红？棋盘格黑白分明？1px 线清晰还是糊？");
  display.powerOff();
}

void loop()
{
  delay(30000);
  Serial.printf("idle vbat = %.3f V, heap = %u\n", readVbat(), ESP.getFreeHeap());
}
