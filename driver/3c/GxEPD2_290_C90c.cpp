// Display Library for SPI e-paper panels from Dalian Good Display and boards from Waveshare.
// Requires HW SPI and Adafruit_GFX. Caution: these e-papers require 3.3V supply AND data lines!
//
// based on Demo Example from Good Display: http://www.e-paper-display.com/download_list/downloadcategoryid=34&isMode=false.html
// Controller: SSD1680 : https://v4.cecdn.yun300.cn/100001_1909185148/SSD1680.pdf
//
// Author: Jean-Marc Zingg
//
// Version: see library.properties
//
// Library: https://github.com/ZinggJM/GxEPD2

#include "GxEPD2_290_C90c.h"

GxEPD2_290_C90c::GxEPD2_290_C90c(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, HIGH, 30000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

void GxEPD2_290_C90c::clearScreen(uint8_t value)
{
  clearScreen(value, 0xFF);
}

void GxEPD2_290_C90c::clearScreen(uint8_t black_value, uint8_t color_value)
{
  _initial_write = false; // initial full screen buffer clean done
  _Init_Part();
  _setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x24);
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _writeData(black_value);
  }
  _writeCommand(0x26);
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _writeData(~color_value);
  }
  _Update_Part();
}

void GxEPD2_290_C90c::writeScreenBuffer(uint8_t value)
{
  writeScreenBuffer(value, 0xFF);
}

void GxEPD2_290_C90c::writeScreenBuffer(uint8_t black_value, uint8_t color_value)
{
  _initial_write = false; // initial full screen buffer clean done
  _Init_Part();
  _setPartialRamArea(0, 0, WIDTH, HEIGHT);
  _writeCommand(0x24);
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _writeData(black_value);
  }
  _writeCommand(0x26);
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _writeData(~color_value);
  }
}

void GxEPD2_290_C90c::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, NULL, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_290_C90c::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
  int16_t wb = (w + 7) / 8; // width bytes, bitmaps are padded
  x -= x % 8; // byte boundary
  w = wb * 8; // byte boundary
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  _Init_Part();
  _setPartialRamArea(x1, y1, w1, h1);
  _writeCommand(0x24);
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data = 0xFF;
      if (black)
      {
        // use wb, h of bitmap for index!
        int16_t idx = mirror_y ? j + dx / 8 + ((h - 1 - (i + dy))) * wb : j + dx / 8 + (i + dy) * wb;
        if (pgm)
        {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
          data = pgm_read_byte(&black[idx]);
#else
          data = black[idx];
#endif
        }
        else
        {
          data = black[idx];
        }
        if (invert) data = ~data;
      }
      _writeData(data);
    }
  }
  _writeCommand(0x26);
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data = 0xFF;
      if (color)
      {
        // use wb, h of bitmap for index!
        int16_t idx = mirror_y ? j + dx / 8 + ((h - 1 - (i + dy))) * wb : j + dx / 8 + (i + dy) * wb;
        if (pgm)
        {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
          data = pgm_read_byte(&color[idx]);
#else
          data = color[idx];
#endif
        }
        else
        {
          data = color[idx];
        }
        if (invert) data = ~data;
      }
      _writeData(~data);
    }
  }
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
}

void GxEPD2_290_C90c::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                 int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, NULL, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_290_C90c::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                 int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  int16_t wb_bitmap = (w_bitmap + 7) / 8; // width bytes, bitmaps are padded
  x_part -= x_part % 8; // byte boundary
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w; // limit
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h; // limit
  x -= x % 8; // byte boundary
  w = 8 * ((w + 7) / 8); // byte boundary, bitmaps are padded
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_using_partial_mode) _Init_Part();
  _setPartialRamArea(x1, y1, w1, h1);
  _writeCommand(0x24);
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data;
      // use wb_bitmap, h_bitmap of bitmap for index!
      int16_t idx = mirror_y ? x_part / 8 + j + dx / 8 + ((h_bitmap - 1 - (y_part + i + dy))) * wb_bitmap : x_part / 8 + j + dx / 8 + (y_part + i + dy) * wb_bitmap;
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&black[idx]);
#else
        data = black[idx];
#endif
      }
      else
      {
        data = black[idx];
      }
      if (invert) data = ~data;
      _writeData(data);
    }
  }
  _writeCommand(0x26);
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data = 0xFF;
      if (color)
      {
        // use wb_bitmap, h_bitmap of bitmap for index!
        int16_t idx = mirror_y ? x_part / 8 + j + dx / 8 + ((h_bitmap - 1 - (y_part + i + dy))) * wb_bitmap : x_part / 8 + j + dx / 8 + (y_part + i + dy) * wb_bitmap;
        if (pgm)
        {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
          data = pgm_read_byte(&color[idx]);
#else
          data = color[idx];
#endif
        }
        else
        {
          data = color[idx];
        }
        if (invert) data = ~data;
      }
      _writeData(~data);
    }
  }
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
}

void GxEPD2_290_C90c::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1)
  {
    writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_290_C90c::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

void GxEPD2_290_C90c::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

void GxEPD2_290_C90c::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(black, color, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

void GxEPD2_290_C90c::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(black, color, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

void GxEPD2_290_C90c::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeNative(data1, data2, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

void GxEPD2_290_C90c::refresh(bool partial_update_mode)
{
  if (partial_update_mode) refresh(0, 0, WIDTH, HEIGHT);
  else _Update_Full();
}

void GxEPD2_290_C90c::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
  // intersection with screen
  int16_t w1 = x < 0 ? w + x : w; // reduce
  int16_t h1 = y < 0 ? h + y : h; // reduce
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  w1 = x1 + w1 < int16_t(WIDTH) ? w1 : int16_t(WIDTH) - x1; // limit
  h1 = y1 + h1 < int16_t(HEIGHT) ? h1 : int16_t(HEIGHT) - y1; // limit
  if ((w1 <= 0) || (h1 <= 0)) return; 
  // make x1, w1 multiple of 8
  w1 += x1 % 8;
  if (w1 % 8 > 0) w1 += 8 - w1 % 8;
  x1 -= x1 % 8;
  _Init_Part();
  _setPartialRamArea(x1, y1, w1, h1);
  _Update_Part();
}

void GxEPD2_290_C90c::powerOff()
{
  _PowerOff();
}

void GxEPD2_290_C90c::hibernate()
{
  _PowerOff();
  if (_rst >= 0)
  {
    _writeCommand(0x10); // deep sleep mode
    _writeData(0x1);     // enter deep sleep
    _hibernating = true;
  }
}

void GxEPD2_290_C90c::setVcom(uint8_t v)
{
  // SSD1680: 0x2C = Write VCOM register
  // 必须在 _Init_Full()/_Init_Part() 之后写入，否则被 SWRESET 冲掉；
  // 在 0x20 (Master Activation) 之前写入即被锁存。
  _writeCommand(0x2C);
  _writeData(v);
}

void GxEPD2_290_C90c::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  _writeCommand(0x44);
  _writeData(x / 8);
  _writeData((x + w - 1) / 8);
  _writeCommand(0x45);
  _writeData(y % 256);
  _writeData(y / 256);
  _writeData((y + h - 1) % 256);
  _writeData((y + h - 1) / 256);
  _writeCommand(0x4e);
  _writeData(x / 8);
  _writeCommand(0x4f);
  _writeData(y % 256);
  _writeData(y / 256);
}

void GxEPD2_290_C90c::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommand(0x22);
    _writeData(0xf8);
    _writeCommand(0x20);
    _waitWhileBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

void GxEPD2_290_C90c::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommand(0x22);
    _writeData(0x83);
    _writeCommand(0x20);
    _waitWhileBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
}

void GxEPD2_290_C90c::_InitDisplay()
{
  if (_hibernating) _reset();
  delay(10); // 10ms according to specs
  _writeCommand(0x12);  //SWRESET
  delay(10); // 10ms according to specs
  _writeCommand(0x01); //Driver output control
  // ★ 区域刷新：GDR 直接决定门线数。写 RAM 之前就要设对，
  //   否则地址映射按 296 行算、扫描按 h 行算，图像会错位。
  if (_gate_lines && (_gate_lines < HEIGHT))
  {
    uint16_t gdr = _gate_lines - 1;
    _writeData(gdr & 0xFF);
    _writeData((gdr >> 8) & 0x01);
    _writeData(0x00);
  }
  else
  {
    _writeData(0x27);
    _writeData(0x01);
    _writeData(0x00);
  }
  _writeCommand(0x11); //data entry mode       
  _writeData(0x03);
  _writeCommand(0x3C); //BorderWavefrom
  _writeData(0x05);  
  _writeCommand(0x18); //Read built-in temperature sensor
  _writeData(0x80);  
  _writeCommand(0x21); //  Display update control
  _writeData(0x00);  
  _writeData(0x80);  
  _setPartialRamArea(0, 0, WIDTH, HEIGHT);
}

void GxEPD2_290_C90c::_Init_Full()
{
  _InitDisplay();
}

void GxEPD2_290_C90c::_Init_Part()
{
  _InitDisplay();
}

void GxEPD2_290_C90c::_Update_Full()
{
  // ★ 区域刷新：只扫描前 _gate_lines 行（0x01 GDR）。
  //   必须在这里写 —— _InitDisplay() 每次都会把 GDR 重置成全屏 295。
  if (_gate_lines && (_gate_lines < HEIGHT))
  {
    uint16_t gdr = _gate_lines - 1;
    _writeCommand(0x01);
    _writeData(gdr & 0xFF);
    _writeData((gdr >> 8) & 0x01);
    _writeData(0x00);
  }
  // ★ 温度覆写：让 SSD1680 按指定温度去 OTP 挑波形（温度越高帧数越少、越快）
  if (_fast_temp)
  {
    _writeCommand(0x1A);
    _writeData(_fast_temp);
  }
  _writeCommand(0x22);
  _writeData(_update_mode);
  _writeCommand(0x20);
  _waitWhileBusy("_Update_Full", full_refresh_time);
  _power_is_on = false;
}

void GxEPD2_290_C90c::_Update_Part()
{
  if (_gate_lines && (_gate_lines < HEIGHT))
  {
    uint16_t gdr = _gate_lines - 1;
    _writeCommand(0x01);
    _writeData(gdr & 0xFF);
    _writeData((gdr >> 8) & 0x01);
    _writeData(0x00);
  }
  if (_fast_temp)
  {
    _writeCommand(0x1A);
    _writeData(_fast_temp);
  }
  _writeCommand(0x22);
  _writeData(_update_mode);
  _writeCommand(0x20);
  _waitWhileBusy("_Update_Part", partial_refresh_time);
  _power_is_on = false;
}

// ============================================================
// 纯黑白快刷实验接口
// ------------------------------------------------------------
// SSD1680 的 0x22 位定义（和 IL3820 同源）：
//   0x80 EN_CLK  0x40 EN_CP  0x20 LOAD_TEMP  0x10 LOAD_LUT(OTP)
//   0x08 MODE2   0x04 MODE1  0x02/0x01 DISPLAY
// 0x10 一置位，芯片就用 OTP 里的波形覆盖 0x32 写进去的 LUT。
// 所以「自定义 LUT + 0xCC」才有意义：0xCC 不含 0x10 也不含 0x20。
// ============================================================

void GxEPD2_290_C90c::expWriteLut(const uint8_t* lut, uint16_t len)
{
  _writeCommand(0x32);
  for (uint16_t i = 0; i < len; i++) _writeData(lut[i]);
}

void GxEPD2_290_C90c::expActivate(uint8_t mode, uint32_t timeout_ms)
{
  _writeCommand(0x22);
  _writeData(mode);
  _writeCommand(0x20);
  _waitWhileBusy("expActivate", timeout_ms);
  // 标记为上电状态：0x22 里含 EN_CLK/EN_ANALOG 时芯片确实是上电的。
  // 若这里标成 false，后续任何内部 _PowerOn() 都会重发 0xF8（含 LOAD_LUT），
  // 把 0x32 写进去的自定义 LUT 覆盖掉 —— 那是"写完 LUT 就不生效"的隐形杀手。
  _power_is_on = true;
}

void GxEPD2_290_C90c::expFillPlane(uint8_t plane_cmd, uint8_t value)
{
  _writeCommand(plane_cmd);
  for (uint32_t i = 0; i < (uint32_t)WIDTH * HEIGHT / 8; i++) _writeData(value);
}

void GxEPD2_290_C90c::expRaw(uint8_t cmd, uint8_t data)
{
  _writeCommand(cmd);
  _writeData(data);
}

// ---------------------------------------------------------------------------
// 裸通道：为什么不能用 writeNative / writeImage
//   1) writeNative(data1, data2, ...) 的实现只转发 data1，data2（红色平面）
//      被整个丢弃 —— 三色屏的 0x26 永远不更新，屏上不会有正确画面。
//   2) writeImage 内部调用 _Init_Part()，而 _Init_Part() == _InitDisplay()，
//      里面有 0x12 SWRESET —— 每次上传都软复位，把 0x32 写进去的自定义 LUT
//      清得一干二净。所以"写完 LUT 再上传数据"这个顺序必然失败：
//      LUT 被复位清空，0x22=0xCC 只能拿到一张全零波形表，12 相空转 112ms，
//      墨水一点不动。
// 正确顺序：设窗口 -> 写 0x24 -> 写 0x26 -> 0x32 写 LUT -> 0x22 触发。
// 全程不调用任何 GxEPD2 高层函数。
// ---------------------------------------------------------------------------

void GxEPD2_290_C90c::expSetRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  _writeCommand(0x11); // data entry mode
  _writeData(0x03);    // x inc, y inc
  _writeCommand(0x44); // set RAM X address range
  _writeData(x / 8);
  _writeData((x + w - 1) / 8);
  _writeCommand(0x45); // set RAM Y address range
  _writeData(y % 256);
  _writeData(y / 256);
  _writeData((y + h - 1) % 256);
  _writeData((y + h - 1) / 256);
  _writeCommand(0x4e); // set RAM X address counter
  _writeData(x / 8);
  _writeCommand(0x4f); // set RAM Y address counter
  _writeData(y % 256);
  _writeData(y / 256);
}

void GxEPD2_290_C90c::expWriteRam(uint8_t cmd, const uint8_t* data, uint32_t len)
{
  _writeCommand(cmd);
  for (uint32_t i = 0; i < len; i++) _writeData(data[i]);
}

// ---------------------------------------------------------------------------
// 直接写 0x01 GDR（门线数）
// 三个字节：低 8 位、第 9 位（MUX 最大 296，所以只用 1 bit）、VDH/VDL 选择。
// lines 会被夹到 1..296；传 0 或超范围按整屏处理。
// ---------------------------------------------------------------------------
void GxEPD2_290_C90c::expSetGdr(uint16_t lines)
{
  if (lines == 0 || lines > HEIGHT) lines = HEIGHT;
  uint16_t gdr = lines - 1;
  _writeCommand(0x01);
  _writeData(gdr & 0xFF);
  _writeData((gdr >> 8) & 0x01);
  _writeData(0x00);
}
