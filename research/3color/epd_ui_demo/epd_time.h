#pragma once
/*
 * epd_time.h —— 简易走时
 *
 * 现在没有联网，时间靠编译时注入的 BUILD_EPOCH（Unix 秒，UTC）。
 * tools/build.py 会自动把「电脑当前时间」塞进 -DBUILD_EPOCH=xxx，
 * 所以每次烧录完，屏上的时间就是烧录那一刻的电脑时间，之后用 millis() 走时。
 * 以后接了 NTP，只要把 clockSetEpoch() 换成网络对时即可，上层代码不用动。
 */

#include <Arduino.h>
#include <stdint.h>

#ifndef BUILD_EPOCH
#define BUILD_EPOCH 0
#endif

#ifndef TZ_OFFSET_HOUR
#define TZ_OFFSET_HOUR 8          /* 北京时间 */
#endif

typedef struct {
  int16_t year;
  uint8_t mon, day, hour, min, sec, wday;   /* wday: 0=周日 */
} ClockTime;

static uint32_t _tmBootMs = 0;
static uint32_t _tmEpoch  = 0;

/* Howard Hinnant 的 civil_from_days：把「1970 起的天数」拆成年月日 */
static inline void civilFromDays(int32_t z, int16_t &y, uint8_t &m, uint8_t &d)
{
  z += 719468;
  int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int16_t)(yoe + era * 400);
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp  = (5 * doy + 2) / 153;
  d = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
  m = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) y++;
}

static inline void clockBegin()
{
  _tmBootMs = millis();
#if BUILD_EPOCH
  _tmEpoch = (uint32_t)BUILD_EPOCH;
  Serial.printf("[时钟] 起点 %lu（编译时注入）\r\n", (unsigned long)_tmEpoch);
#else
  _tmEpoch = 1794369600UL;    /* 兜底：2026-11-11 12:00:00 +08，仅用于调试 */
  Serial.println(F("[时钟] 未注入 BUILD_EPOCH，时间不准，请用 tools/build.py 编译"));
#endif
}

/* 运行中校准（以后接 NTP 直接调用） */
static inline void clockSetEpoch(uint32_t utc)
{
  _tmEpoch  = utc;
  _tmBootMs = millis();
}

static inline uint32_t epochNow()
{
  return _tmEpoch + (millis() - _tmBootMs) / 1000 + (uint32_t)TZ_OFFSET_HOUR * 3600UL;
}

static inline ClockTime clockRead()
{
  ClockTime t;
  uint32_t e    = epochNow();
  uint32_t days = e / 86400UL;
  uint32_t rem  = e % 86400UL;

  civilFromDays((int32_t)days, t.year, t.mon, t.day);
  t.hour = (uint8_t)(rem / 3600UL);
  t.min  = (uint8_t)((rem / 60UL) % 60UL);
  t.sec  = (uint8_t)(rem % 60UL);
  t.wday = (uint8_t)((days + 4) % 7UL);    /* 1970-01-01 是周四 */
  return t;
}
