/*
 * epd_play v2 —— A01 (IL3820) 墨水屏高速播放器
 *
 * 相比 v1 的变化：
 *   1. 波特率 921600 -> 2000000（传输 1216B 从 10.6ms 降到 4.9ms）
 *   2. TP 可通过串口实时切换，不用重烧（30fps 用 TP=3，60fps 用 TP=1）
 *   3. 全刷间隔可通过串口设置，且改用 partial 波形（348ms，比 clearScreen 快 5 倍）
 *   4. 包协议带类型字节，方便扩展
 *
 * 波形关键（实测，别改）：
 *   LUT VS = 0x18  双向驱动。0x10 只能单向，旧图案擦不掉，残影会累积成反相白斑。
 *   TFRAME = 296 行 × 31us = 9.18ms    单帧 = TP × 9.18ms + 上传 1.4ms
 *
 * 协议（PC -> ESP8266）统一包头 0xA5 0x5A，第三字节为类型：
 *   'F' len_lo len_hi <RLE 数据>   上传并显示一帧
 *   'W'                            快速全刷（清残影）
 *   'T' <tp>                       设置每帧时间片数（1..15）
 *   'N' <n_lo> <n_hi>              设置自动全刷间隔（帧数，0=关闭）
 *   'Q'                            查询状态，回 "S,<tp>,<frames>,<washes>"
 *
 * 应答（ESP8266 -> PC）：
 *   'R' 帧已刷新完毕    'W' 全刷完毕    'E' 出错
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
#define BAUD     2000000UL

static uint8_t fb[FBBYTES] __attribute__((aligned(4)));
static uint8_t pkt[4096];
static uint8_t lut[30];

// 出厂 partial 波形（sumTP=19 -> 174ms），用来快速清残影
static const uint8_t LUT_PART30[30] = {
  0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0x13, 0x14, 0x44, 0x12, 0, 0, 0, 0, 0, 0
};

static uint8_t  gTP        = 3;
static uint16_t gWashEvery = 600;
static uint32_t gFrames    = 0;
static uint32_t gWashes    = 0;

// ------------------------------------------------------------------ 波形

static void setFastLut()
{
  memset(lut, 0, 30);
  lut[0]  = 0x18;        // 双向：字段1 = -VSL（黑→白），字段2 = +VSH（白→黑）
  lut[20] = gTP & 0x0F;
  display.epd2.expSetLut(lut, 30);
  display.epd2.expSetGateTime(0x00, 0x00);
  display.epd2.expSetMux(GATES);
  display.epd2.expCpOn();
}

// 快速全刷：用 partial 波形刷白。
// 实测教训：连做两次 174ms 不间断会触发 USB 过流保护掉电（30fps 播放中尤其明显）。
// 所以拆成「断电歇一下 -> 刷一次 -> 断电再歇」，把持续驱动切成两段。
static void fastWash()
{
  display.epd2.expSetLut((uint8_t *)LUT_PART30, 30);
  memset(fb, 0xFF, FBBYTES);
  display.epd2.expUploadImageFast(fb, FBBYTES);
  display.epd2.expUploadOldFast(fb, FBBYTES);   // 0x26 也写白，否则比较基准是脏的

  ExpTiming t;
  display.epd2.expActivateTimed(0x04, 4000, &t);
  display.epd2.expPowerOff();                   // 关升压，让稳压器和电荷泵喘口气
  delay(250);
  setFastLut();                                 // 重新开泵 + 装回快刷波形
  display.epd2.expActivateTimed(0x04, 4000, &t);

  gWashes++;
}

// ------------------------------------------------------------------ RLE

static void rleDecode(const uint8_t *d, uint16_t n)
{
  uint16_t o = 0, i = 0;
  while (i < n && o < FBBYTES) {
    uint8_t h = d[i++];
    uint16_t cnt = h & 0x7F;
    if (cnt == 0) cnt = 128;
    if (h & 0x80) {
      if (i >= n) break;
      uint8_t v = d[i++];
      while (cnt-- && o < FBBYTES) fb[o++] = v;
    } else {
      while (cnt-- && i < n && o < FBBYTES) fb[o++] = d[i++];
    }
  }
  while (o < FBBYTES) fb[o++] = 0xFF;
}

// ------------------------------------------------------------------ 收包

static inline bool waitHead()
{
  static uint8_t prev = 0;
  uint32_t idle = 0;
  while (true) {
    if (Serial.available()) {
      uint8_t b = (uint8_t)Serial.read();
      if (prev == 0xA5 && b == 0x5A) { prev = 0; return true; }
      prev = b;
      idle = 0;
    } else if (++idle > 4000000UL) {
      return false;
    }
  }
}

static inline bool waitBytes(uint8_t *dst, uint16_t n)
{
  uint16_t got = 0;
  uint32_t stall = 0;
  while (got < n) {
    if (Serial.available()) {
      dst[got++] = (uint8_t)Serial.read();
      stall = 0;
    } else if (++stall > 4000000UL) {
      return false;
    }
  }
  return true;
}

void setup()
{
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Serial.begin(BAUD);
  delay(200);

  display.init(0, true, 2, false);
  display.epd2.selectSPI(SPI, SPISettings(40000000, MSBFIRST, SPI_MODE0));

  // 上电走一次完整 clearScreen，把上一轮所有残影彻底清掉
  display.clearScreen(0xFF);
  setFastLut();

  memset(fb, 0xFF, FBBYTES);
  Serial.write('R');
}

void loop()
{
  if (!waitHead()) return;

  // 第三字节：类型
  uint8_t tb;
  if (!waitBytes(&tb, 1)) { Serial.write('E'); return; }

  switch (tb) {

  case 'F': {
    uint8_t lb[2];
    if (!waitBytes(lb, 2)) { Serial.write('E'); return; }
    uint16_t len = (uint16_t)lb[0] | ((uint16_t)lb[1] << 8);
    if (len == 0 || len > sizeof(pkt)) { Serial.write('E'); return; }
    if (!waitBytes(pkt, len)) { Serial.write('E'); return; }

    rleDecode(pkt, len);
    display.epd2.expUploadImageFast(fb, FBBYTES);
    ExpTiming t;
    display.epd2.expActivateTimed(0x04, 3000, &t);
    gFrames++;

    if (gWashEvery && (gFrames % gWashEvery) == 0) {
      fastWash();          // 注意：这里会顺带把 fb 清成白，下一帧重新填满即可
    }
    Serial.write('R');
    break;
  }

  case 'W':
    fastWash();
    Serial.write('W');
    break;

  case 'T': {
    uint8_t v;
    if (!waitBytes(&v, 1)) { Serial.write('E'); return; }
    if (v < 1) v = 1;
    if (v > 15) v = 15;
    gTP = v;
    setFastLut();
    Serial.write('R');
    break;
  }

  case 'N': {
    uint8_t nb[2];
    if (!waitBytes(nb, 2)) { Serial.write('E'); return; }
    gWashEvery = (uint16_t)nb[0] | ((uint16_t)nb[1] << 8);
    Serial.write('R');
    break;
  }

  case 'Q':
    Serial.printf("\r\nS,%u,%lu,%lu\r\n", gTP,
                  (unsigned long)gFrames, (unsigned long)gWashes);
    break;

  default:
    Serial.write('E');
    break;
  }
}
