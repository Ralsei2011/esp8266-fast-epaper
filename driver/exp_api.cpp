/*
 * exp_api.cpp —— EPD_EXP_DECLS 的实现
 *
 * 放在 GxEPD2/src/epd/ 目录下，Arduino 会连同库一起编译。
 * 这些是 GxEPD2_290 的成员函数，所以可以直接访问 _writeCommand / _busy 等私有成员。
 */

#include "GxEPD2_290.h"

void GxEPD2_290::expInit()
{
  _reset();
  delay(10);
  _InitDisplay();
  _power_is_on = false;
  _using_partial_mode = false;
  _hibernating = false;
}

void GxEPD2_290::expSetLut(const uint8_t* lut, uint16_t n)
{
  _writeCommand(0x32);
  _writeData(lut, n);
}

void GxEPD2_290::expAnalogOn()
{
  _writeCommand(0x22);
  _writeData(0xc0);   // 开时钟 + 开模拟，但不做 display
  _writeCommand(0x20);
  _waitWhileBusy("expAnalogOn", power_on_time);
  _power_is_on = true;
}

void GxEPD2_290::expScan(uint32_t timeout_ms)
{
  _writeCommand(0x22);
  _writeData(0x04);   // 只 display：不重载 LUT、不重载温度
  _writeCommand(0x20);
  _waitWhileBusy("expScan", timeout_ms);
}

void GxEPD2_290::expPowerOff()
{
  _writeCommand(0x22);
  _writeData(0xc3);
  _writeCommand(0x20);
  _waitWhileBusy("expPowerOff", power_off_time);
  _power_is_on = false;
}

// 0x3b = Gatetime（每条 gate 的驱动时长），0x3a = DummyLine
void GxEPD2_290::expSetGateTime(uint8_t gatetime, uint8_t dummyline)
{
  _writeCommand(0x3b);
  _writeData(gatetime);
  _writeCommand(0x3a);
  _writeData(dummyline);
}

void GxEPD2_290::expScanMode(uint8_t mode, uint32_t timeout_ms)
{
  _writeCommand(0x22);
  _writeData(mode);
  _writeCommand(0x20);
  _waitWhileBusy("expScanMode", timeout_ms);
  if (!(mode & 0x40)) _power_is_on = false;   // 没置 analog 位就当作已断电
}

void GxEPD2_290::expSetMux(uint16_t n)
{
  if (n < 1) n = 1;
  if (n > HEIGHT) n = HEIGHT;
  _writeCommand(0x01);
  _writeData((n - 1) % 256);
  _writeData((n - 1) / 256);
  _writeData(0x00);
}

// bit4 = WS_Status：为 1 时用 OTP 里的波形，忽略 0x32 写进去的 LUT
void GxEPD2_290::expSetOtpSel(uint8_t v)
{
  _writeCommand(0x37);
  _writeData(v);
}

void GxEPD2_290::expClockOn()
{
  _writeCommand(0x22);
  _writeData(0x80);
  _writeCommand(0x20);
  delay(2);
}

void GxEPD2_290::expCpOn()
{
  _writeCommand(0x22);
  _writeData(0xC0);
  _writeCommand(0x20);
  _waitWhileBusy("expCpOn", power_on_time);
  _power_is_on = true;
}

void GxEPD2_290::expAllOff()
{
  _writeCommand(0x22);
  _writeData(0x03);
  _writeCommand(0x20);
  delay(5);
  _power_is_on = false;
}

// 温度寄存器 12 bit：A = 高 8 位，B[7:4] = 低 4 位，单位 1/16 °C
void GxEPD2_290::expWriteTemp(uint16_t raw12)
{
  _writeCommand(0x1A);
  _writeData((uint8_t)((raw12 >> 4) & 0xFF));
  _writeData((uint8_t)((raw12 & 0x0F) << 4));
}

void GxEPD2_290::expLoadTempFromSensor()
{
  _writeCommand(0x1D);
  delay(2);
}

// 只写显存，绝不碰 LUT / 定时寄存器（库的 writeImage 会顺带 _Init_Part）
void GxEPD2_290::expUploadImage(const uint8_t* data, uint16_t bytes)
{
  _setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x24);
  _startTransfer();
  for (uint16_t i = 0; i < bytes; i++) _transfer(data[i]);
  _endTransfer();
}

// 逐字节 transfer() 在 ESP8266 上每次约 2.4us，4736 字节要 11.5ms；
// writeBytes 批量发同量数据只要 1.2ms，快约 9 倍。
// 注意：data 必须是 4 字节对齐，否则 ESP8266 会抛 Exception 9（LoadStoreAlignmentCause）。
void GxEPD2_290::expUploadImageFast(const uint8_t* data, uint16_t bytes)
{
  _setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x24);
  _startTransfer();
  _pSPIx->writeBytes(data, bytes);
  _endTransfer();
}

// 0x26 是「旧帧」显存。全刷时 IC 会拿 0x24 与 0x26 比较来决定驱动方向，
// 只写 0x24 会让旧帧一直是脏的 —— 这是「全刷刷不白」的直接原因。
void GxEPD2_290::expUploadOldFast(const uint8_t* data, uint16_t bytes)
{
  _setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x26);
  _startTransfer();
  _pSPIx->writeBytes(data, bytes);
  _endTransfer();
}

void GxEPD2_290::expTriggerOnly(uint8_t mode)
{
  _writeCommand(0x22);
  _writeData(mode);
  _writeCommand(0x20);
}

// 精确计时 + BUSY 边沿统计。
// 三种收尾条件：正常结束（BUSY 释放）/ 从未拉起 BUSY（视为空操作）/ 超时。
void GxEPD2_290::expActivateTimed(uint8_t mode, uint32_t timeout_ms, ExpTiming* out)
{
  out->edges = 0;
  out->high_us = 0;
  out->start_us = 0;
  out->total_us = 0;
  out->busy_seen = false;
  out->timeout = false;

  expTriggerOnly(mode);

  uint32_t t0 = micros();
  uint8_t last = digitalRead(_busy);
  uint32_t tChange = t0;
  bool busyOn = (last == (uint8_t)_busy_level);
  if (busyOn)
  {
    out->busy_seen = true;
    out->start_us = 0;
  }

  const uint32_t kWaitBusyUs = 80000UL;   // 80ms 内没抬起 BUSY 就认为这一档是空操作
  const uint32_t limitUs = timeout_ms * 1000UL;
  uint32_t guard = 0;

  while (1)
  {
    uint8_t lvl = digitalRead(_busy);
    uint32_t now = micros();
    if (lvl != last)
    {
      out->edges++;
      if (lvl == (uint8_t)_busy_level)
      {
        busyOn = true;
        if (!out->busy_seen) out->start_us = now - t0;
        out->busy_seen = true;
      }
      else
      {
        busyOn = false;
        out->high_us += now - tChange;
      }
      tChange = now;
      last = lvl;
    }
    if (out->busy_seen && !busyOn) break;
    if (!out->busy_seen && (now - t0) > kWaitBusyUs) break;
    if ((now - t0) > limitUs)
    {
      out->timeout = true;
      break;
    }
    if ((++guard & 0x7FF) == 0) yield();
  }
  uint32_t t1 = micros();
  if (busyOn) out->high_us += t1 - tChange;
  out->total_us = t1 - t0;
}

void GxEPD2_290::expRaw(uint8_t cmd, const uint8_t* data, uint16_t n)
{
  _writeCommand(cmd);
  for (uint16_t i = 0; i < n; i++) _writeData(data[i]);
}
