#pragma once
/*
 * ============================================================
 *  epd_panel.h —— 墨水屏适配层（自己写的，不复制第三方固件）
 * ============================================================
 *
 *  驱动全部来自 GxEPD2 公开库；型号、排线丝印、驱动 IC 的对应关系
 *  只是「一张元数据表」，来自公开的产品说明，不含任何第三方固件代码。
 *  初始化序列、LUT、波形全部由 GxEPD2 提供，我们不改。
 *
 *  用法：改下面一行 EPD_PANEL 选型号，编译即可。
 *        未选中的驱动不会被链接进来，flash 只多占一份。
 *
 *  本机实测：P29_C90_3C（SSD1680 三色屏，busy 高有效，128x296）
 */

#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>

/* ---------------- 型号编号（2.9 寸，我们的板子只能插 2.9） ---------------- */
#define P29_A01      1   /* 排线 A01    SSD1608 / IL3820       黑白 */
#define P29_T5       2   /* 排线 Z10    UC8151C / IL0373       黑白 */
#define P29_T5D      3   /* 排线 Z10    UC8151D                黑白 */
#define P29_T94      4   /*             SSD1680                黑白 */
#define P29_T94V2    5   /*             SSD1680 V2 变体        黑白 */
#define P29_M06      6   /*             GDEW029M06             黑白 */
#define P29_I6FD     7   /*             GDEW029I6FD            黑白 */
#define P29_BS       8   /*             GDEW029BS             黑白 */
#define P29_Z10_3C   9   /* 排线 Z10    GDEW029Z10  三色       */
#define P29_C90_3C  10   /* ★ 本机      SSD1680     三色       */
#define P29_Z13_3C  11   /*             GDEW029Z13  三色       */

/* ================= 选择屏幕型号（只改这一行） ================= */
#ifndef EPD_PANEL
#define EPD_PANEL P29_C90_3C
#endif
/* =========================================================== */

/* ---------------- 各型号的驱动映射 ---------------- */

#if EPD_PANEL == P29_A01
  #define EPD_NAME    "A01"
  #define EPD_IC      "SSD1608/IL3820"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290
  #define EPD_FASTPAR 0        /* A01 是黑白屏，库自带快刷 LUT，无需温度注入 */

#elif EPD_PANEL == P29_T5
  #define EPD_NAME    "T5"
  #define EPD_IC      "UC8151C/IL0373"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_T5
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_T5D
  #define EPD_NAME    "T5D"
  #define EPD_IC      "UC8151D"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_T5D
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_T94
  #define EPD_NAME    "T94"
  #define EPD_IC      "SSD1680"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_T94
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_T94V2
  #define EPD_NAME    "T94V2"
  #define EPD_IC      "SSD1680"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_T94_V2
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_M06
  #define EPD_NAME    "M06"
  #define EPD_IC      "GDEW029M06"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_M06
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_I6FD
  #define EPD_NAME    "I6FD"
  #define EPD_IC      "GDEW029I6FD"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_I6FD
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_BS
  #define EPD_NAME    "BS"
  #define EPD_IC      "GDEW029BS"
  #define EPD_COLORS  2
  #define EPD_DRIVER  GxEPD2_290_BS
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_Z10_3C
  #define EPD_NAME    "Z10"
  #define EPD_IC      "GDEW029Z10"
  #define EPD_COLORS  3
  #define EPD_DRIVER  GxEPD2_290c
  #define EPD_FASTPAR 0

#elif EPD_PANEL == P29_C90_3C
  #define EPD_NAME    "C90"
  #define EPD_IC      "SSD1680"
  #define EPD_COLORS  3
  #define EPD_DRIVER  GxEPD2_290_C90c
  #define EPD_FASTPAR 1        /* 我们给这个驱动加了温度/门线注入接口 */

#elif EPD_PANEL == P29_Z13_3C
  #define EPD_NAME    "Z13"
  #define EPD_IC      "GDEW029Z13"
  #define EPD_COLORS  3
  #define EPD_DRIVER  GxEPD2_290_Z13c
  #define EPD_FASTPAR 0

#else
  #error "EPD_PANEL 取值不认识，请用 P29_xxx 常量"
#endif

/* ---------------- 显示对象类型 ---------------- */

#if EPD_COLORS == 3
  #define EPD_DISPLAY_T  GxEPD2_3C<EPD_DRIVER, EPD_DRIVER::HEIGHT>
  #define C_RED          GxEPD_RED
#else
  #define EPD_DISPLAY_T  GxEPD2_BW<EPD_DRIVER, EPD_DRIVER::HEIGHT>
  #define C_RED          GxEPD_BLACK   /* 黑白屏没有红色层，红按黑画 */
#endif

/*
 * 支持清单（屏上"屏幕支持"页会逐条列出）
 * fastParam: 1 = 驱动里带了 0x1A 温度覆写 / 0x01 门线 / 0x22 波形注入接口
 *            这种屏能跑我们的 3 秒竖带快刷；0 = 只能按库默认参数刷
 */
typedef struct {
  uint8_t     id;
  const char *name;   /* 丝印 / 型号 */
  const char *ic;     /* 驱动 IC */
  uint8_t     colors; /* 2 黑白 / 3 三色 */
  uint8_t     fast;   /* 是否支持快刷参数注入 */
} PanelEntry;

static const PanelEntry PANEL_LIST[] = {
  { P29_C90_3C, "C90",   "SSD1680",       3, 1 },
  { P29_Z10_3C, "Z10",   "GDEW029Z10",    3, 0 },
  { P29_Z13_3C, "Z13",   "GDEW029Z13",    3, 0 },
  { P29_A01,    "A01",   "SSD1608/IL3820",2, 1 },
  { P29_T5,     "T5",    "UC8151C/IL0373",2, 0 },
  { P29_T5D,    "T5D",   "UC8151D",       2, 0 },
  { P29_T94,    "T94",   "SSD1680",       2, 0 },
  { P29_T94V2,  "T94V2", "SSD1680",       2, 0 },
  { P29_M06,    "M06",   "GDEW029M06",    2, 0 },
  { P29_I6FD,   "I6FD",  "GDEW029I6FD",   2, 0 },
  { P29_BS,     "BS",    "GDEW029BS",     2, 0 },
};
#define PANEL_COUNT (sizeof(PANEL_LIST) / sizeof(PanelEntry))

/*
 * ---------------- 其它尺寸参考（本板 PCB 只能插 2.9 寸，故未启用） ----------------
 * 换板子时照上面的写法加一段 #if 分支即可，驱动同样来自 GxEPD2：
 *
 *   1.54" 200x200   GxEPD2_154 / GxEPD2_154c / GxEPD2_154_Z90c
 *                  对应 WFC0000CZ07、HINK-E0154A05、中景园 SSD1681
 *   2.13" 122x250   GxEPD2_213 / GxEPD2_213c / GxEPD2_213_Z19c / GxEPD2_213_Z98c
 *                  对应 GDE0213B1
 *   2.66" 152x296   GxEPD2_266c
 *                  对应 HINK-E0266A15-A0
 *   4.2"  400x300   GxEPD2_420 / GxEPD2_420c / GxEPD2_420c_Z21
 *                  对应 WFH0420CZ35、HINK-E04A13-A0(Z96)、Z98
 *
 * 注意：分辨率不同的屏不能共用同一套版式，UI 里 W/H/DYN_W 都要跟着改。
 */

