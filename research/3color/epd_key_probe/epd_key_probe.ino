/*
 * 全 IO 按键扫描 v7 —— 不预设任何引脚，11 个脚一起盯
 *
 * 前情：
 *   V14 源码写 key_zj=GPIO0 / key_yb=GPIO5
 *   README 说按键1=复位、按键2=GPIO0、按键3=GPIO5，V2.3X 硬件要改到 GPIO3
 *   但实测只有 GPIO0 有反应，GPIO3/GPIO5 全是高，用户按了三个键都没用
 *   → 不再猜，全扫一遍
 *
 * 扫描范围（GPIO6-11 是 Flash 总线，绝对不碰，碰了直接崩）：
 *   G0  / 屏幕DC      G1  / TX        G2  / 屏幕RST    G3  / RX
 *   G4  / 屏幕BUSY    G5  / SD_CS     G12 / MISO&电池开关
 *   G13 / MOSI&I2C-SDA  G14 / SCK&I2C-SCL  G15 / 屏幕CS  G16 / D0
 *
 * 做法：全部切成 INPUT_PULLUP 读电平，记基线，之后任何下降沿都计数。
 * 停手 1.5 秒后刷屏把结果摆出来；期间串口同步打印谁动了。
 * 注意 G1 每次切完必须立刻 pinMode(1, FUNCTION_0) 否则串口发不出去。
 */

#include <GxEPD2_3C.h>

#define PIN_CS   15
#define PIN_DC    0
#define PIN_RST   2
#define PIN_BUSY  4

#define W  296
#define H  128

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
  display(GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));

// ================= 扫描表 =================
static const uint8_t PINS[] = { 0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16 };
static const char *TAGS[] = {
  "G0 DC", "G1 TX", "G2 RST", "G3 RX", "G4 BUSY", "G5 SD",
  "G12", "G13", "G14", "G15 CS", "G16"
};
#define NP (sizeof(PINS) / sizeof(PINS[0]))

static uint8_t  baseLvl[NP];   // 开机基线
static uint8_t  lastLvl[NP];   // 上一轮电平
static uint16_t cnt[NP];       // 下降沿次数
static volatile int8_t hitIdx = -1;

static uint32_t lastEvtMs = 0;
static bool     dirty = false;

// ================= 引脚状态恢复 =================
static void restoreAll()
{
  pinMode(1, FUNCTION_0);            // TX 恢复串口
  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  pinMode(5, INPUT_PULLUP);          // SD_CS 空闲拉高
}

// 读一个脚（切输入 → 读 → 由 restoreAll 统一恢复）
static inline uint8_t readPin(uint8_t i)
{
  pinMode(PINS[i], INPUT_PULLUP);
  delayMicroseconds(12);
  return digitalRead(PINS[i]) ? 1 : 0;
}

// 一轮完整扫描（含恢复）
static void scanAll(uint8_t *out)
{
  for (uint8_t i = 0; i < NP; i++) out[i] = readPin(i);
  restoreAll();
}

// ================= 画面 =================
static void drawScreen(uint8_t *cur, const char *hint)
{
  display.fillScreen(GxEPD_WHITE);

  // 标题条
  display.fillRect(0, 0, W, 13, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.print("ALL-IO SCAN");
  display.setCursor(96, 3);
  if (hitIdx >= 0) {
    char b[24]; sprintf(b, "HIT %s", TAGS[hitIdx]);
    display.print(b);
  } else {
    display.print("no hit yet");
  }
  display.setCursor(210, 3);
  display.print(hint);

  // 两列 11 项
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  for (uint8_t i = 0; i < NP; i++) {
    uint8_t col = (i < 6) ? 0 : 1;
    uint8_t row = (i < 6) ? i : (i - 6);
    int16_t x = 4 + col * 150;
    int16_t y = 16 + row * 18;

    // 电平块：低电平涂黑，一眼看出「正在被拉低」
    display.fillRect(x, y, 7, 9, cur[i] ? GxEPD_WHITE : GxEPD_BLACK);
    display.drawRect(x, y, 7, 9, GxEPD_BLACK);

    display.setCursor(x + 11, y);
    display.print(TAGS[i]);

    // 计数：非零标红，方便一眼找到被按的那个
    char b[8]; sprintf(b, "%u", cnt[i]);
    if (cnt[i] > 0) {
      display.setTextColor(GxEPD_RED);
      display.setCursor(x + 96, y);
      display.print(b);
      display.setTextColor(GxEPD_BLACK);
    } else {
      display.setCursor(x + 96, y);
      display.print(b);
    }
  }

  // 底部提示
  display.drawFastHLine(0, H - 12, W, GxEPD_BLACK);
  display.setCursor(4, H - 10);
  display.print("press each button slowly; screen updates 1.5s after you stop");
}

static void flushScreen(const char *hint)
{
  uint8_t cur[NP];
  scanAll(cur);

  restoreAll();
  display.epd2.setFastTemp(100);
  display.epd2.setUpdateMode(0xD7);
  display.epd2.setGateLines(0);
  display.setFullWindow();
  display.firstPage();
  do { drawScreen(cur, hint); } while (display.nextPage());
}

// ================= 主流程 =================
void setup()
{
  Serial.begin(115200);
  delay(400);

  display.init(115200, true, 20, false);
  display.setRotation(3);

  // 基线
  scanAll(baseLvl);
  for (uint8_t i = 0; i < NP; i++) {
    lastLvl[i] = baseLvl[i];
    cnt[i] = 0;
  }

  Serial.println(F(""));
  Serial.println(F("===== 全 IO 扫描 v7 ====="));
  Serial.print(F("脚位基线: "));
  for (uint8_t i = 0; i < NP; i++) {
    Serial.printf("%s=%d ", TAGS[i], baseLvl[i]);
  }
  Serial.println();
  Serial.println(F("现在请把三个键各按几次（长按短按都行），停手 1.5 秒后屏幕会更新。"));

  flushScreen("READY");

  // 清掉刷屏本身造成的假事件
  scanAll(lastLvl);
  hitIdx = -1;
  for (uint8_t i = 0; i < NP; i++) cnt[i] = 0;
  dirty = false;
}

void loop()
{
  static uint32_t lastLog = 0;
  uint8_t cur[NP];

  scanAll(cur);

  for (uint8_t i = 0; i < NP; i++) {
    if (cur[i] != lastLvl[i]) {
      if (cur[i] == 0) {              // 下降沿
        cnt[i]++;
        hitIdx = i;
        lastEvtMs = millis();
        dirty = true;
        Serial.printf(">>> %s 按下  cnt=%u\r\n", TAGS[i], cnt[i]);
      } else {
        Serial.printf("    %s 松开\r\n", TAGS[i]);
      }
      lastLvl[i] = cur[i];
    }
  }

  // 停手 1.5 秒后刷一屏
  if (dirty && millis() - lastEvtMs > 1500) {
    dirty = false;
    Serial.println(F("--- 刷屏汇总 ---"));
    flushScreen("HIT");
    scanAll(lastLvl);          // 刷屏期间的抖动不计数
  }

  if (millis() - lastLog > 8000) {
    lastLog = millis();
    Serial.print(F("[等] "));
    for (uint8_t i = 0; i < NP; i++) Serial.printf("%s:%u ", TAGS[i], cnt[i]);
    Serial.println();
  }
  delay(3);
}
