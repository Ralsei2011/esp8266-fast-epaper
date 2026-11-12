/*
 * exp_api.h —— 给 GxEPD2_290 追加的低层寄存器接口
 *
 * 为什么需要它：
 *   GxEPD2 的公开 API 每一步都会带上 _Init_Part / _Init_Full 这些副作用，
 *   会重新下发 LUT、温度、扫描时序，把我们要保持的状态全部覆盖掉。
 *   想做「只改一个寄存器、然后单独触发一次扫描」这种实验，必须直接写命令。
 *
 * 集成方式（一共两处、两行）：
 *   1. 把 exp_api.h / exp_api.cpp 拷进 GxEPD2/src/epd/
 *   2. 在 GxEPD2_290.h 里：
 *        #include "exp_api.h"          // 放在 class 声明之前
 *        class GxEPD2_290 : public GxEPD2_EPD
 *        {
 *        public:
 *          EPD_EXP_DECLS              // 放在 public 区
 *          ...
 *
 * 这样声明和实现都留在库外面，不必改动库原有的任何一行代码。
 */

#ifndef _EPD_EXP_API_H_
#define _EPD_EXP_API_H_

#include <stdint.h>

// 一次刷新的实测结果
struct ExpTiming
{
  uint32_t total_us;   // 从写完 0x20 到 BUSY 释放
  uint32_t high_us;    // BUSY 累计为「忙」的时长
  uint32_t start_us;   // 到 BUSY 第一次抬起的延迟
  uint16_t edges;      // BUSY 电平跳变次数（1=一次连续忙；>1=有脉冲）
  bool     busy_seen;  // 这次是否真的拉起了 BUSY
  bool     timeout;    // 是否超时放弃
};

#define EPD_EXP_DECLS                                                              \
  void expInit();                                                                  \
  void expSetLut(const uint8_t* lut, uint16_t n);      /* 0x32 写 n 字节波形 */     \
  void expAnalogOn();                                  /* 0x22=0xC0 只开升压 */      \
  void expScan(uint32_t timeout_ms = 2000);            /* 0x22=0x04 只显示 */       \
  void expPowerOff();                                  /* 0x22=0xC3 关升压 */       \
  void expSetGateTime(uint8_t gatetime, uint8_t dummyline); /* 0x3b / 0x3a */       \
  void expScanMode(uint8_t mode, uint32_t timeout_ms = 2000); /* 任意 0x22 值 */    \
  void expSetMux(uint16_t n);                          /* 0x01 有效门线数 */        \
  void expSetOtpSel(uint8_t v);                        /* 0x37 OTP 波形选择 */      \
  void expClockOn();                                   /* 0x22=0x80 仅开时钟 */     \
  void expCpOn();                                      /* 0x22=0xC0 开钟+电荷泵 */   \
  void expAllOff();                                    /* 0x22=0x03 关泵关钟 */     \
  void expWriteTemp(uint16_t raw12);                   /* 0x1A 温度寄存器 */        \
  void expLoadTempFromSensor();                        /* 0x1D 载入传感器温度 */    \
  void expUploadImage(const uint8_t* data, uint16_t bytes);     /* 0x24 逐字节 */   \
  void expUploadImageFast(const uint8_t* data, uint16_t bytes); /* 0x24 批量 */     \
  void expUploadOldFast(const uint8_t* data, uint16_t bytes);   /* 0x26 旧帧 */     \
  void expTriggerOnly(uint8_t mode);                   /* 发 0x22/0x20 不等 BUSY */  \
  void expActivateTimed(uint8_t mode, uint32_t timeout_ms, ExpTiming* out);         \
  void expRaw(uint8_t cmd, const uint8_t* data, uint16_t n);

#endif
