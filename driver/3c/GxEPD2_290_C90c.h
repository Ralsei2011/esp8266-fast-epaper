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

#ifndef _GxEPD2_290_C90c_H_
#define _GxEPD2_290_C90c_H_

#include "../GxEPD2_EPD.h"

class GxEPD2_290_C90c : public GxEPD2_EPD
{
  public:
    // attributes
    static const uint16_t WIDTH = 128;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 296;
    static const GxEPD2::Panel panel = GxEPD2::GDEM029C90;
    static const bool hasColor = true;
    static const bool hasPartialUpdate = true; // but refresh is full screen
    static const bool hasFastPartialUpdate = false;
    static const uint16_t power_on_time = 100; // ms, e.g. 94628us
    static const uint16_t power_off_time = 150; // ms, e.g. 139874us
    static const uint16_t full_refresh_time = 27000; // ms, e.g. 26130024us
    static const uint16_t partial_refresh_time = 27000; // ms, e.g. 26130024us
    // constructor
    GxEPD2_290_C90c(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
    // methods (virtual)
    //  Support for Bitmaps (Sprites) to Controller Buffer and to Screen
    void clearScreen(uint8_t value = 0xFF); // init controller memory and screen (default white)
    void clearScreen(uint8_t black_value, uint8_t color_value); // init controller memory and screen
    void writeScreenBuffer(uint8_t value = 0xFF); // init controller memory (default white)
    void writeScreenBuffer(uint8_t black_value, uint8_t color_value); // init controller memory
    // write to controller memory, without screen refresh; x and w should be multiple of 8
    void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write sprite of native data to controller memory, without screen refresh; x and w should be multiple of 8
    void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write to controller memory, with screen refresh; x and w should be multiple of 8
    void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write sprite of native data to controller memory, with screen refresh; x and w should be multiple of 8
    void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void refresh(bool partial_update_mode = false); // screen refresh from controller memory to full screen
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h); // screen refresh from controller memory, partial screen
    void powerOff(); // turns off generation of panel driving voltages, avoids screen fading over time
    void hibernate(); // turns powerOff() and sets controller to deep sleep for minimum power use, ONLY if wakeable by RST (rst >= 0)
    // 追加：显式写 VCOM (0x2C)。原驱动完全依赖 OTP 默认值，
    // 若面板批次与 OTP 不匹配就会对比度不足（发淡）。
    // 注意：必须在 firstPage()/_Init_Full() 之后、nextPage() 之前调用才生效。
    void setVcom(uint8_t v);
    // ★ 加速钩子：把面板 OTP 里烧死的长波形换掉
    //   setFastTemp(t): 写 0x1A 温度覆写寄存器，让 SSD1680 去 OTP 里挑 t℃ 那条波形。
    //                   t=0 表示不覆写（回到读内部温度传感器的默认行为）。
    //                   实测 100℃ 满屏 5.1s / 65℃ 满屏 11.2s / 默认 19.4~27.2s。
    //   setUpdateMode(m): 改 _Update_Full/Part 里的 0x22 参数。默认 0xF7（三色唯一正式路径），
    //                   配合温度覆写用 0xD7（跳过重新读温度）。
    //   setGateLines(n): 把 0x01 的 GDR 改成 n-1，即只扫描前 n 行。
    //                   刷新耗时 ≈ 2.86s + 55.8ms × n。必须与 setPartialWindow 的 h 一致。
    void setFastTemp(uint8_t t) { _fast_temp = t; }
    void setUpdateMode(uint8_t m) { _update_mode = m; }
    void setGateLines(uint16_t n) { _gate_lines = n; }
    // ★ 纯黑白快刷实验接口（三色屏当黑白屏用）
    //   思路：SSD1680 的 0x32 可以写 153 字节自定义 LUT，0x22 的 bit4(0x10)
    //   才会用 OTP 覆盖它。所以写 LUT 后必须用不含 0x10 的模式（如 0xCC）触发。
    //   三色驱动从来没走过这条路，全程 0xF7 吃 OTP 多相波形 —— 这就是 27 秒的来源。
    void expWriteLut(const uint8_t* lut, uint16_t len); // 0x32 写自定义 LUT
    void expActivate(uint8_t mode, uint32_t timeout_ms); // 0x22=mode + 0x20 + 等忙
    void expFillPlane(uint8_t plane_cmd, uint8_t value); // 0x24/0x26 整面填充
    void expRaw(uint8_t cmd, uint8_t data);              // 单字节命令，调试用
    // ★ 绕开 writeNative 的裸通道（writeNative 会丢红色平面，且内部 SWRESET 清 LUT）
    void expSetRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h); // 0x11/0x44/0x45/0x4e/0x4f
    void expWriteRam(uint8_t cmd, const uint8_t* data, uint32_t len);   // 0x24 或 0x26 裸写
    // ★ 直接写 0x01 GDR（门线数）。
    //   GDR 只决定"扫多少行"，扫描永远从 gate 0 开始 —— 这是判断
    //   "只有半边屏能刷"到底是物理限制还是配置问题的关键寄存器。
    //   lines=296 为整屏，lines=148 表示只驱动 gate 0..147。
    void expSetGdr(uint16_t lines);
  private:
    uint8_t _fast_temp = 0;      // 0 = 不覆写温度
    uint8_t _update_mode = 0xF7; // 默认全刷波形
    uint16_t _gate_lines = 0;    // 0 = 全屏 296 行
    void _writeScreenBuffer(uint8_t value);
    void _setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _PowerOn();
    void _PowerOff();
    void _InitDisplay();
    void _Init_Full();
    void _Init_Part();
    void _Update_Full();
    void _Update_Part();
};

#endif
