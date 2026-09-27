// st7789.c
#include "st7789.h"
#include "delay.h"
#include "font_ascii_8x16.h"
#include "irq_guard.h"
#include <intrins.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

// LCD分辨率定义（240x240）
static unsigned int LCD_WIDTH = 240;
static unsigned int LCD_HEIGHT = 240;

/**
 * Write command to LCD (8080 interface)
 * Standard timing: CS=0, RS=0, DB=cmd, WR↓, WR↑, CS=1
 */
void LCM_Write_Command(unsigned char cmd)
{
    LCM_CS = 0;      // CS拉低
    LCM_RS = 0;      // 命令模式
    
    LCM_DB = cmd;    // 设置数据
    _nop_(); _nop_(); _nop_(); _nop_(); // 数据建立时间（至少50ns，增加延时）
    
    LCM_WR = 0;      // WR拉低（锁存数据）
    _nop_(); _nop_(); _nop_(); _nop_(); // WR脉冲宽度（至少50ns）
    LCM_WR = 1;      // WR拉高
    
    _nop_(); _nop_(); // 数据保持时间
    LCM_CS = 1;      // CS拉高
}

/**
 * Write 8-bit data to LCD WITHOUT CS control (for continuous write)
 * Internal function: assumes CS is already controlled by caller
 * Standard timing: RS=1, DB=data, WR↓, WR↑
 */
static void LCM_Write_Data8_NoCS(unsigned char dat)
{
    LCM_RS = 1;      // 数据模式
    
    LCM_DB = dat;    // 设置数据
    _nop_(); _nop_(); _nop_(); _nop_(); // 数据建立时间（至少50ns，增加延时）
    
    LCM_WR = 0;      // WR拉低（锁存数据）
    _nop_(); _nop_(); _nop_(); _nop_(); // WR脉冲宽度（至少50ns）
    LCM_WR = 1;      // WR拉高
    
    _nop_(); _nop_(); // 数据保持时间
}

/**
 * Write 8-bit data to LCD (8080 interface)
 * Public interface with CS control for single-byte writes
 */
void LCM_Write_Data(unsigned char dat)
{
    LCM_CS = 0;      // CS拉低
    LCM_Write_Data8_NoCS(dat);
    LCM_CS = 1;      // CS拉高
}

/**
 * Write 16-bit data (RGB565)
 */
void LCM_Write_Data16(unsigned int dat)
{
    LCM_Write_Data(dat >> 8);    // 高字节
    LCM_Write_Data(dat & 0xFF);  // 低字节
}

#if 0
/**
 * Initialize LCD backlight (P3.4, TP8006 driver)
 * Configures P3.4 as push-pull output and turns on backlight
 */
void LCM_Init_Backlight(void)
{
    // Configure P3.4 as push-pull output for backlight
    P3M0 |= 0x10;   // Set P3M0 bit4 = 1 (P3.4 push-pull output)
    P3M1 &= ~0x10;  // Clear P3M1 bit4 = 0
    
    // Turn on backlight (TP8006 requires >2.5V on DIM pin)
    LCM_PWM = 1;
}
#endif

/**
 * Reset LCD
 */
void LCM_Reset(void)
{
    LCM_RST = 0;      // 拉低复位
    Delay_ms(50);     // 复位低电平时间（增加到50ms，更可靠）
    LCM_RST = 1;      // 拉高
    Delay_ms(120);    // 等待复位完成（ST7789要求至少120ms）
}

/**
 * Complete initialization for ST7789 240x240
 * Includes full ST77xx initialization sequence with Gamma correction
 */
void LCM_Init(void)
{
    // ============================================
    // GPIO配置（重要：直接赋值，不要按位或）
    // ============================================
    // P2口：数据总线（D0-D7）
    P2M0 = 0xFF;  // P2全部推挽输出
    P2M1 = 0x00;
    P2 = 0x00;    // 初始化为0
    
    // P4口：控制线（P4.2=WR, P4.3=CS, P4.4=RD, P4.5=RS, P4.6=RST）
    P4M0 = 0x7C;  // P4.2-P4.6推挽输出（直接赋值，避免被其他模块影响）
    P4M1 = 0x00;  // 确保其他位不受影响
    
    // ============================================
    // 初始化控制引脚状态（重要：RD必须永远拉高！）
    // ============================================
    LCM_RD = 1;    // RD永远保持高电平（某些COG屏RD被拉低会死屏）
    LCM_CS = 1;    // CS初始高电平（不选中）
    LCM_WR = 1;    // WR初始高电平（不写）
    LCM_RS = 0;    // RS初始低电平（命令模式）
    
    Delay_ms(100); // 等待电源稳定
    
    // 硬件复位
    LCM_Reset();
    
    // ============================================
    // 完整ST77xx初始化序列（COG TFT必须）
    // ============================================
    
    // Sleep Out（必须！没发这个永远黑屏）
    LCM_Write_Command(0x11);
    Delay_ms(120);  // ST7789要求至少120ms
    
    // Display Inversion On（修正黑白反转问题）
    LCM_Write_Command(0x21);
    
    // Pixel Format = RGB565（16位）
    LCM_Write_Command(0x3A);
    LCM_Write_Data(0x55);  // 0x55 = 16-bit RGB565
    
    // Porch Control（帧同步参数）
    LCM_Write_Command(0xB2);
    LCM_Write_Data(0x0C);
    LCM_Write_Data(0x0C);
    LCM_Write_Data(0x00);
    LCM_Write_Data(0x33);
    LCM_Write_Data(0x33);
    
    // Gate Control
    LCM_Write_Command(0xB7);
    LCM_Write_Data(0x35);
    
    // VCOM Setting
    LCM_Write_Command(0xBB);
    LCM_Write_Data(0x19);
    
    // LCM Control
    LCM_Write_Command(0xC0);
    LCM_Write_Data(0x2C);
    
    // VDV and VRH Command Enable
    LCM_Write_Command(0xC2);
    LCM_Write_Data(0x01);
    
    // VRH Set
    LCM_Write_Command(0xC3);
    LCM_Write_Data(0x12);
    
    // VDV Set
    LCM_Write_Command(0xC4);
    LCM_Write_Data(0x20);
    
    // Frame Rate Control
    LCM_Write_Command(0xC6);
    LCM_Write_Data(0x0F);
    
    // Power Control 1
    LCM_Write_Command(0xD0);
    LCM_Write_Data(0xA4);
    LCM_Write_Data(0xA1);
    
    // Positive Voltage Gamma Control
    LCM_Write_Command(0xE0);
    LCM_Write_Data(0xD0);
    LCM_Write_Data(0x04);
    LCM_Write_Data(0x0D);
    LCM_Write_Data(0x11);
    LCM_Write_Data(0x13);
    LCM_Write_Data(0x2B);
    LCM_Write_Data(0x3F);
    LCM_Write_Data(0x54);
    LCM_Write_Data(0x4C);
    LCM_Write_Data(0x18);
    LCM_Write_Data(0x0D);
    LCM_Write_Data(0x0B);
    LCM_Write_Data(0x1F);
    LCM_Write_Data(0x23);
    
    // Negative Voltage Gamma Control
    LCM_Write_Command(0xE1);
    LCM_Write_Data(0xD0);
    LCM_Write_Data(0x04);
    LCM_Write_Data(0x0C);
    LCM_Write_Data(0x11);
    LCM_Write_Data(0x13);
    LCM_Write_Data(0x2C);
    LCM_Write_Data(0x3F);
    LCM_Write_Data(0x44);
    LCM_Write_Data(0x51);
    LCM_Write_Data(0x2F);
    LCM_Write_Data(0x1F);
    LCM_Write_Data(0x1F);
    LCM_Write_Data(0x20);
    LCM_Write_Data(0x23);
    
    // Normal Display Mode On
    LCM_Write_Command(0x13);
    Delay_ms(10);
    
    // Memory Access Control（BGR顺序，修正色彩）
    // Bit 3 (0x08): BGR order (instead of RGB)
    LCM_Write_Command(0x36);
    LCM_Write_Data(0x08);  // BGR顺序
    Delay_ms(10);
    
    // Display ON
    LCM_Write_Command(0x29);
    Delay_ms(50);  // Display ON后需要足够延时
}

/**
 * Set display window (Column and Row Address)
 */
void LCM_Set_Window(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2)
{
    // 列地址设置（0x2A）
    LCM_Write_Command(0x2A);
    LCM_Write_Data16(x1);
    LCM_Write_Data16(x2);
    
    // 行地址设置（0x2B）
    LCM_Write_Command(0x2B);
    LCM_Write_Data16(y1);
    LCM_Write_Data16(y2);
    
    // 写显存命令（0x2C）
    LCM_Write_Command(0x2C);
}

#if 0
/**
 * Write pixel (16-bit RGB565 color)
 * Uses LCM_Write_Data8_NoCS for continuous write (no CS control)
 */
void LCM_Write_Pixel(unsigned int color)
{
    // 高字节
    LCM_RS = 1;
    LCM_DB = color >> 8;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 0;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 1;
    _nop_(); _nop_();
    
    // 低字节
    LCM_RS = 1;
    LCM_DB = color & 0xFF;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 0;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 1;
    _nop_(); _nop_();
}
#endif

/**
 * Fill entire screen with color
 * CS is controlled once for the entire fill operation
 */
void LCM_Fill_Color(unsigned int color)
{
    unsigned long i;
    unsigned long total = (unsigned long)LCD_WIDTH * LCD_HEIGHT;
    
    // 设置窗口为全屏
    LCM_Set_Window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    
    // 连续写入像素数据（CS保持低，提高速度）
    LCM_CS = 0;
    LCM_RS = 1;  // 数据模式
    
    for(i = 0; i < total; i++)
    {
        // 高字节
        LCM_DB = color >> 8;
        _nop_(); _nop_(); _nop_(); _nop_(); _nop_(); _nop_();
        LCM_WR = 0;
        _nop_(); _nop_(); _nop_(); _nop_(); _nop_(); _nop_();
        LCM_WR = 1;
        _nop_(); _nop_(); _nop_(); _nop_();
        
        // 低字节
        LCM_DB = color & 0xFF;
        _nop_(); _nop_(); _nop_(); _nop_(); _nop_(); _nop_();
        LCM_WR = 0;
        _nop_(); _nop_(); _nop_(); _nop_(); _nop_(); _nop_();
        LCM_WR = 1;
        _nop_(); _nop_(); _nop_(); _nop_();
    }
    
    LCM_CS = 1;  // CS拉高，结束传输
}

#if 0
/**
 * Set display direction (MADCTL)
 */
void LCM_Set_MADCTL(unsigned char madctl)
{
    LCM_Write_Command(0x36);
    LCM_Write_Data(madctl);
    Delay_ms(10);
}
#endif

#if 0
/**
 * Set resolution
 */
void LCM_Set_Resolution(unsigned int width, unsigned int height)
{
    LCD_WIDTH = width;
    LCD_HEIGHT = height;
}
#endif

#if 0
/**
 * Get resolution
 */
void LCM_Get_Resolution(unsigned int *width, unsigned int *height)
{
    *width = LCD_WIDTH;
    *height = LCD_HEIGHT;
}
#endif

/**
 * Draw a single pixel at (x, y) with color
 */
void LCM_Draw_Pixel(unsigned int x, unsigned int y, unsigned int color)
{
    if(x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    
    LCM_Set_Window(x, y, x, y);
    
    LCM_CS = 0;
    LCM_RS = 1;
    
    // 高字节
    LCM_DB = color >> 8;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 0;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 1;
    _nop_(); _nop_();
    
    // 低字节
    LCM_DB = color & 0xFF;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 0;
    _nop_(); _nop_(); _nop_(); _nop_();
    LCM_WR = 1;
    _nop_(); _nop_();
    
    LCM_CS = 1;
}

// ============================================
// 通用字符绘制函数 - 支持任意尺寸字体
// ============================================

/**
 * 通用字符绘制函数
 * 
 * @param x X坐标
 * @param y Y坐标
 * @param font_data 字模数据指针（逐行式，顺向，阴码）
 * @param font_width 字体宽度（像素）
 * @param font_height 字体高度（像素）
 * @param fg_color 前景色（RGB565）
 * @param bg_color 背景色（RGB565）
 * 
 * 字模格式说明：
 * - 存储方式：逐行式（Row Major）
 * - 每行数据：从左到右，每8个像素占1字节
 * - 位顺序：MSB(bit7)为最左像素，LSB(bit0)为最右像素
 * - 字节顺序：每行第1字节存储第1-8列，第2字节存储第9-16列...
 * - 总字节数：((font_width + 7) / 8) * font_height
 * 
 * 支持任意尺寸字体，例如：
 * - 8x16: 每行1字节，共16字节
 * - 16x32: 每行2字节，共64字节
 * - 24x24: 每行3字节，共72字节
 * - 42x42: 每行6字节，共252字节
 */
void LCM_Draw_Char_Generic(unsigned int x, unsigned int y, const unsigned char *font_data,
                           unsigned char font_width, unsigned char font_height,
                           unsigned int fg_color, unsigned int bg_color)
{
    unsigned char row, col;
    unsigned char bytes_per_row;
    unsigned char byte_idx, bit_idx;
    unsigned int pixel_color;
    unsigned char byte_val;
    
    // 参数检查
    if(font_data == NULL || font_width == 0 || font_height == 0)
    {
        return;
    }
    
    // 计算每行的字节数
    bytes_per_row = (font_width + 7) / 8;  // 向上取整
    
    // 逐行绘制
    for(row = 0; row < font_height; row++)
    {
        for(col = 0; col < font_width; col++)
        {
            // 计算当前像素对应的字节索引和位索引
            byte_idx = col / 8;                    // 第几个字节 (0, 1, 2, ...)
            bit_idx = 7 - (col % 8);               // 字节内的位 (7-0, MSB在左)
            
            // 读取字节数据
            byte_val = font_data[row * bytes_per_row + byte_idx];
            
            // 判断像素是否点亮
            pixel_color = (byte_val & (1 << bit_idx)) ? fg_color : bg_color;
            
            // 绘制像素
            LCM_Draw_Pixel(x + col, y + row, pixel_color);
        }
    }
}

/**
 * 通用字符串绘制函数
 * 
 * @param x X坐标
 * @param y Y坐标
 * @param str 字符串
 * @param font_getter 字模获取函数指针，接受字符返回字模数据
 * @param font_width 字体宽度（像素）
 * @param font_height 字体高度（像素）
 * @param fg_color 前景色（RGB565）
 * @param bg_color 背景色（RGB565）
 * @param spacing 字符间距（像素）
 * 
 * 使用示例：
 * // 绘制16x32字符串
 * LCM_Draw_String_Generic(0, 0, "Hello", Font_Get16x32, 16, 32, RGB565_WHITE, RGB565_BLACK, 2);
 * 
 * // 绘制8x16字符串
 * LCM_Draw_String_Generic(0, 40, "World", Font_Get8x16, 8, 16, RGB565_RED, RGB565_BLACK, 1);
 */
void LCM_Draw_String_Generic(unsigned int x, unsigned int y, const char *str,
                             const unsigned char* (*font_getter)(char),
                             unsigned char font_width, unsigned char font_height,
                             unsigned int fg_color, unsigned int bg_color,
                             unsigned char spacing)
{
    unsigned int x_pos = x;
    const unsigned char *font_data;
    
    // 参数检查
    if(str == NULL || font_getter == NULL || font_width == 0 || font_height == 0)
    {
        return;
    }
    
    while(*str)
    {
        // 获取字符字模数据
        font_data = font_getter(*str);
        
        // 如果获取到有效字模数据，则绘制
        if(font_data != NULL)
        {
            LCM_Draw_Char_Generic(x_pos, y, font_data, font_width, font_height, fg_color, bg_color);
        }
        // 空格或无效字符仍然占用空间
        
        x_pos += font_width + spacing;  // 移动到下一个字符位置
        str++;
    }
}

// ============================================
// 兼容旧API的快捷函数
// ============================================

/**
 * Draw a 16x32 character (original size, 16x32 pixels)
 * 使用 font_ascii_8x16.h 中的完整 ASCII 字库
 * 
 * 注：此函数保留以兼容现有代码，内部调用通用函数实现
 */
void LCM_Draw_Char_16x32(unsigned int x, unsigned int y, unsigned char ch, unsigned int fg_color, unsigned int bg_color)
{
    const unsigned char *font_data;
    
    // 从字库获取字符点阵数据（支持 ASCII 0x20 ~ 0x7E）
    font_data = Font_Get16x32(ch);
    
    // 如果字符不在支持范围内，不绘制
    if(font_data == NULL)
    {
        return;
    }
    
    // 使用通用函数绘制
    LCM_Draw_Char_Generic(x, y, font_data, FONT_ASCII_WIDTH, FONT_ASCII_HEIGHT, fg_color, bg_color);
}

/**
 * Draw a string (16x32 font, original size)
 * 
 * 注：此函数保留以兼容现有代码，内部调用通用函数实现
 */
void LCM_Draw_String_16x32(unsigned int x, unsigned int y, const char *str, unsigned int fg_color, unsigned int bg_color)
{
    // 使用通用函数绘制，字符间距为2像素
    LCM_Draw_String_Generic(x, y, str, Font_Get16x32, FONT_ASCII_WIDTH, FONT_ASCII_HEIGHT, fg_color, bg_color, 2);
}

#if 0
/**
 * Draw a 8x16 character (2x scale, 16x32 pixels) - Medium size for better visibility
 * 使用 font_ascii_8x16.h 中的完整 ASCII 字库
 * 注：已弃用，现在使用16x32原生字体
 */
void LCM_Draw_Char_8x16_Medium(unsigned int x, unsigned int y, unsigned char ch, unsigned int fg_color, unsigned int bg_color)
{
    unsigned char i, j;
    const unsigned char *font_data;
    
    // 从字库获取字符点阵数据（支持 ASCII 0x20 ~ 0x7E）
    font_data = Font_Get8x16(ch);
    
    // 如果字符不在支持范围内，不绘制
    if(font_data == NULL)
    {
        return;
    }
    
    // 绘制字符（8列x16行，2倍放大变成16列x32行）
    for(i = 0; i < 16; i++)  // 16行（放大到32行）
    {
        unsigned char row_data = font_data[i];
        for(j = 0; j < 8; j++)  // 8列（放大到16列）
        {
            unsigned int px = x + j * 2;      // 水平放大2倍
            unsigned int py = y + i * 2;      // 垂直放大2倍
            unsigned int color;
            
            // 检查像素是否点亮（MSB在左，即第7位是最左边）
            color = (row_data & (0x80 >> j)) ? fg_color : bg_color;
            
            // 绘制2x2像素块（放大2倍）
            LCM_Draw_Pixel(px,     py,     color);  // 左上
            LCM_Draw_Pixel(px + 1, py,     color);  // 右上
            LCM_Draw_Pixel(px,     py + 1, color);  // 左下
            LCM_Draw_Pixel(px + 1, py + 1, color);  // 右下
        }
    }
}

/**
 * Draw a string (8x16 font, 2x scale, 16x32 pixels) - Medium size for better visibility
 * 注：已弃用，现在使用16x32原生字体
 */
void LCM_Draw_String_8x16_Medium(unsigned int x, unsigned int y, const char *str, unsigned int fg_color, unsigned int bg_color)
{
    unsigned int x_pos = x;
    while(*str)
    {
        if(*str != ' ')  // 空格不绘制，但占用空间
        {
            LCM_Draw_Char_8x16_Medium(x_pos, y, *str, fg_color, bg_color);
        }
        x_pos += 18;  // 字符间距18像素（字符宽度16像素 + 2像素间距，避免重叠）
        str++;
    }
}
#endif

/**
 * Draw a horizontal bar graph
 * @param x: Start X position
 * @param y: Start Y position
 * @param width: Bar width in pixels
 * @param height: Bar height in pixels
 * @param value: Current value (0-100)
 * @param max_value: Maximum value (typically 100)
 * @param fg_color: Foreground color (filled part)
 * @param bg_color: Background color (empty part)
 */
void LCM_Draw_BarGraph(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned char value, unsigned char max_value, unsigned int fg_color, unsigned int bg_color)
{
    unsigned int i, j;
    unsigned int fill_width;
    
    // Calculate fill width
    if(value > max_value)
    {
        value = max_value;
    }
    fill_width = (width * value) / max_value;
    
    // Draw bar graph
    for(i = 0; i < height; i++)
    {
        for(j = 0; j < width; j++)
        {
            if(j < fill_width)
            {
                LCM_Draw_Pixel(x + j, y + i, fg_color);
            }
            else
            {
                LCM_Draw_Pixel(x + j, y + i, bg_color);
            }
        }
    }
}

/**
 * Fill a rectangle with color
 * @param x: Start X position
 * @param y: Start Y position
 * @param width: Rectangle width
 * @param height: Rectangle height
 * @param color: Fill color
 */
void LCM_FillRect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color)
{
    unsigned int i, j;
    
    // Boundary check
    if(x + width > 240) width = 240 - x;
    if(y + height > 240) height = 240 - y;
    
    // Set window
    LCM_Set_Window(x, y, x + width - 1, y + height - 1);
    
    // Fill rectangle
    LCM_CS = 0;
    LCM_RS = 1;  // Data mode
    
    for(i = 0; i < height; i++)
    {
        for(j = 0; j < width; j++)
        {
            // High byte
            LCM_DB = color >> 8;
            _nop_(); _nop_(); _nop_(); _nop_();
            LCM_WR = 0;
            _nop_(); _nop_(); _nop_(); _nop_();
            LCM_WR = 1;
            _nop_(); _nop_();
            
            // Low byte
            LCM_DB = color & 0xFF;
            _nop_(); _nop_(); _nop_(); _nop_();
            LCM_WR = 0;
            _nop_(); _nop_(); _nop_(); _nop_();
            LCM_WR = 1;
            _nop_(); _nop_();
        }
    }
    
    LCM_CS = 1;
}

/**
 * Draw a rectangle border
 * @param x: Start X position
 * @param y: Start Y position
 * @param width: Rectangle width
 * @param height: Rectangle height
 * @param color: Border color
 */
void LCM_DrawRect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color)
{
    unsigned int i;
    
    // Boundary check
    if(x + width > 240) width = 240 - x;
    if(y + height > 240) height = 240 - y;
    
    // Top edge
    for(i = 0; i < width; i++)
    {
        LCM_Draw_Pixel(x + i, y, color);
    }
    
    // Bottom edge
    for(i = 0; i < width; i++)
    {
        LCM_Draw_Pixel(x + i, y + height - 1, color);
    }
    
    // Left edge
    for(i = 0; i < height; i++)
    {
        LCM_Draw_Pixel(x, y + i, color);
    }
    
    // Right edge
    for(i = 0; i < height; i++)
    {
        LCM_Draw_Pixel(x + width - 1, y + i, color);
    }
}

/**
 * 绘制水平线
 * @param x 起始X坐标
 * @param y Y坐标
 * @param length 线长度
 * @param color 颜色
 */
void LCM_DrawHLine(unsigned int x, unsigned int y, unsigned int length, unsigned int color)
{
    unsigned int i;
    for(i = 0; i < length; i++)
    {
        LCM_Draw_Pixel(x + i, y, color);
    }
}

/**
 * 显示RGB565格式图像（带Image2Lcd头部）
 * @param x 起始X坐标
 * @param y 起始Y坐标
 * @param image_data 图像数据指针（包含Image2Lcd 8字节头部）
 * @param width 图像宽度
 * @param height 图像高度
 * @param header_size 头部大小（通常为8字节）
 */
void LCM_Draw_Image_RGB565(unsigned int x, unsigned int y, const unsigned char* image_data, 
                           unsigned char width, unsigned char height, unsigned char header_size)
{
    unsigned int i, j;
    unsigned int pixel_rgb, pixel_bgr;
    const unsigned char* pixel_data;
    unsigned char irq_state;
    
    if(image_data == 0)
        return;
    
    // Skip header to get actual image data
    pixel_data = image_data + header_size;
    
    irq_state = IRQ_Save();
    
    // Set window for image drawing
    LCM_Set_Window(x, y, x + width - 1, y + height - 1);
    
    // Write image data pixel by pixel (RGB565 format, 2 bytes per pixel)
    // Note: ST7789 expects BGR565, but image data is in RGB565, so we need to swap R and B
    for(i = 0; i < height; i++)
    {
        for(j = 0; j < width; j++)
        {
            // Read RGB565 pixel (Big Endian: high byte first, low byte second)
            pixel_rgb = ((unsigned int)(*pixel_data++) << 8);  // High byte
            pixel_rgb |= *pixel_data++;                        // Low byte
            
            // Convert RGB565 to BGR565 (swap R and B channels)
            // RGB565: RRRR RGGG GGGB BBBB (R在bit15-11, G在bit10-5, B在bit4-0)
            // BGR565: BBBB BGGG GGGR RRRR (B在bit15-11, G在bit10-5, R在bit4-0)
            // 提取R (bit15-11) 移到 bit4-0，提取B (bit4-0) 移到 bit15-11，G保持不变
            pixel_bgr = ((pixel_rgb & 0xF800) >> 11) |  // 提取R (bit15-11) → 移到bit4-0
                        (pixel_rgb & 0x07E0) |           // G保持不变 (bit10-5)
                        ((pixel_rgb & 0x001F) << 11);    // 提取B (bit4-0) → 移到bit15-11
            
            // Write pixel color in BGR565 format
            LCM_Write_Data16(pixel_bgr);
        }
    }
    
    IRQ_Restore(irq_state);
}

