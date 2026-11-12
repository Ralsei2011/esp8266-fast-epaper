/*
 * 三色屏「快波形画质」对比 —— 一次一档，屏上大字标出档位
 *
 * 之前两次测试都因为方法问题失败：
 *   1. 三档画在同一屏 → 后面的刷新扰动前面的，不可比
 *   2. 手写寄存器通路 → 平面极性写错，满屏红/黑
 * 这次改成：
 *   - 数据通路 100% 用 GxEPD2（已验证能正确出图）
 *   - 一个固件只画一档，整屏擦白后只刷一次
 *   - 档位存 EEPROM，断电重启自增，屏上用大字标出来（不依赖串口）
 *
 * 波形参数通过驱动钩子注入：
 *   setFastTemp(t) → 0x1A 温度覆写，让面板去 OTP 挑 t℃ 的波形
 *   setUpdateMode(m) → 0x22 参数
 *
 * 切下一档：电池开关 OFF 2 秒 → ON
 */

#include <GxEPD2_3C.h>
#include <EEPROM.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

#define EEP_ADDR   0
#define EEP_MAGIC  0xC500

struct Variant {
  uint8_t     temp;    // 0 = 不覆写温度
  uint8_t     mode;    // 0x22 参数
  const char *name;    // 屏上显示的名字
};

// 只放三档。之前实测满屏耗时：BASE 19.4s / 65℃ 11.2s / 100℃ 5.1s
static const Variant VARIANTS[] = {
  {   0, 0xF7, "BASE" },
  {  65, 0xD7, "65C"  },
  { 100, 0xD7, "100C" },
};
static const uint8_t NVAR = sizeof(VARIANTS) / sizeof(VARIANTS[0]);

// ---------------- 测试图案（每档完全一致，只差波形） ----------------
static void drawTest(const char *name)
{
  display.fillScreen(GxEPD_WHITE);

  // 1) 档位名：textSize 3 → 18px 高，反白显示，一眼认出是哪档
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(3);
  display.setCursor(2, 6);
  display.print(name);

  // 2) 纯黑块（整宽 40 行）—— 看黑度，发灰这里最明显
  display.fillRect(0, 40, 128, 40, GxEPD_BLACK);

  // 3) 纯红块（整宽 40 行）—— 看红度，发粉/发淡这里最明显
  display.fillRect(0, 88, 128, 40, GxEPD_RED);

  // 4) 1px 竖线，每 2px 一条 —— 最苛刻的锐度测试，糊了就并成灰面
  for (int16_t x = 0; x < 128; x += 2)
    display.drawFastVLine(x, 136, 30, GxEPD_BLACK);

  // 5) 4x4 棋盘格 —— 中等难度
  for (int16_t y = 0; y < 32; y += 4)
    for (int16_t x = 0; x < 128; x += 4)
      if (((x / 4) + (y / 4)) & 1)
        display.fillRect(x, 172 + y, 4, 4, GxEPD_BLACK);

  // 6) 1px 棋盘格 —— 最难，快波形在这里最容易翻车
  for (int16_t y = 0; y < 32; y++)
    for (int16_t x = 0; x < 128; x++)
      if ((x + y) & 1)
        display.drawPixel(x, 210 + y, GxEPD_BLACK);

  // 7) 3px 粗字 + 1px 细字对照 —— 看小字可读性
  display.setTextSize(2);
  display.setCursor(2, 250);
  display.print("Hello 123");
  display.setTextSize(1);
  display.setCursor(2, 276);
  display.print("The quick brown fox jumps over");
}

// ---------------- 主流程 ----------------
void setup()
{
  Serial.begin(115200);
  delay(300);

  EEPROM.begin(512);
  uint16_t v = 0;
  EEPROM.get(EEP_ADDR, v);
  if ((v & 0xFF00) != EEP_MAGIC) v = EEP_MAGIC;
  uint8_t idx = v & 0x00FF;
  if (idx >= NVAR) idx = 0;

  const Variant &V = VARIANTS[idx];

  Serial.println();
  Serial.printf("#### 档位 %u/%u : %s (temp=%u mode=0x%02X) ####\r\n",
                idx + 1, NVAR, V.name, V.temp, V.mode);

  display.init(115200, true, 20, false);

  // ---- 第一步：用基线波形整屏擦白 ----
  // 必须每次都擦，否则上一档的残留会叠加进来，就变成上次那种「不可比」。
  Serial.println(F("[1] 擦白（基线波形）..."));
  display.epd2.setFastTemp(0);
  display.epd2.setUpdateMode(0xF7);
  display.epd2.setGateLines(0);        // 全屏 296 行
  uint32_t t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
  Serial.printf("    擦白 %lu ms\r\n", (unsigned long)(millis() - t0));

  // ---- 第二步：切到本档波形，刷测试图 ----
  Serial.printf("[2] 测试图（temp=%u mode=0x%02X）...\r\n", V.temp, V.mode);
  display.epd2.setFastTemp(V.temp);
  display.epd2.setUpdateMode(V.mode);
  display.epd2.setGateLines(0);

  t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    drawTest(V.name);
  } while (display.nextPage());
  uint32_t dt = millis() - t0;
  Serial.printf("    测试图 %lu ms\r\n", (unsigned long)dt);

  // 存下一档，下次开机自动前进（这一档跑完就切走，用户只需拨一次开关）
  EEPROM.put(EEP_ADDR, (uint16_t)(EEP_MAGIC | ((idx + 1) % NVAR)));
  EEPROM.commit();

  Serial.printf(">>> %s = %lu ms   heap=%u\r\n",
                V.name, (unsigned long)dt, ESP.getFreeHeap());
  Serial.println(F("DONE — 看屏幕记下这一档，然后关电池开关 2 秒再开 → 下一档"));

  display.hibernate();
}

void loop() { delay(1000); }
