// st7789.h

#include <STC8H.H>
#ifndef __ST7789_H__
#define __ST7789_H__

// ============================================
// 引脚定义（8080并口标准）
// ============================================
// 数据总线（8位）
#define LCM_DB  P2      // P2.0 ~ P2.7 (D0-D7)

// 控制线
sbit LCM_WR  = P4^2;    // WR (写使能)
sbit LCM_CS  = P4^3;    // CS (片选)
sbit LCM_RD  = P4^4;    // RD (读使能，通常保持高电平)
sbit LCM_RS  = P4^5;    // RS/DC (0=命令, 1=数据)
sbit LCM_RST = P4^6;    // RST (复位)

// 背光
sbit LCM_PWM = P3^4;    // BL/LED (背光控制)

// RGB565颜色定义（常用颜色）
#define RGB565_BLACK     0x0000  // 黑色
#define RGB565_WHITE     0xFFFF  // 白色
#define RGB565_RED       0xF800  // 红色
#define RGB565_GREEN     0x07E0  // 绿色
#define RGB565_BLUE      0x001F  // 蓝色
#define RGB565_CYAN      0x07FF  // 青色
#define RGB565_MAGENTA   0xF81F  // 品红
#define RGB565_YELLOW    0xFFE0  // 黄色

// RGB565颜色转换宏
#define RGB565(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

// 函数声明
void LCM_Init(void);
#if 0
void LCM_Init_Backlight(void);  // Initialize LCD backlight (P3.4, TP8006)
#endif
void LCM_Write_Command(unsigned char cmd);
void LCM_Write_Data(unsigned char dat);
void LCM_Write_Data16(unsigned int dat);
void LCM_Reset(void);
void LCM_Set_Window(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2);
void LCM_Fill_Color(unsigned int color);
#if 0
void LCM_Write_Pixel(unsigned int color);
void LCM_Set_MADCTL(unsigned char madctl);
void LCM_Set_Resolution(unsigned int width, unsigned int height);
void LCM_Get_Resolution(unsigned int *width, unsigned int *height);
#endif
void LCM_Draw_Pixel(unsigned int x, unsigned int y, unsigned int color);
void LCM_Draw_Char_8x16_Medium(unsigned int x, unsigned int y, unsigned char ch, unsigned int fg_color, unsigned int bg_color);
void LCM_Draw_String_8x16_Medium(unsigned int x, unsigned int y, const char *str, unsigned int fg_color, unsigned int bg_color);
void LCM_Draw_BarGraph(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned char value, unsigned char max_value, unsigned int fg_color, unsigned int bg_color);
void LCM_FillRect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color);
void LCM_DrawRect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color);
void LCM_DrawHLine(unsigned int x, unsigned int y, unsigned int length, unsigned int color);

#endif
