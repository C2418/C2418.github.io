// ui.c
// 料位仪监控界面实现

#include "ui.h"
#include "ui_theme.h"
#include "st7789.h"
#include "font_ascii_8x16.h"
#include "Config.h"
#include "irq_guard.h"
#include "NE2.h"
#include <intrins.h>

// ================= 内部辅助函数 =================

// ================= 数值转换常量 =================
#define DECIMAL_BASE       10
#define FLOAT_DECIMAL_DIGITS  1   // 浮点数小数位数
#define MAX_DIGITS         16     // 最大数字位数

// ================= 数值显示位置常量 =================
// 左侧列（温度、湿度、位置、传感器）
#define LABEL_WIDTH        54   // "RH:" 标签宽度（约3个16px字符）
#define VALUE_X            (UI_LEFT_MARGIN + LABEL_WIDTH)  // 数值起始X
#define VALUE_WIDTH        70   // 数值显示宽度（容纳4个16px字符+小数点，约65px）
#define UNIT_X             (VALUE_X + VALUE_WIDTH + 4)     // 单位起始X（增加间距）
#define PERCENT_X          168  // 百分比数据起始X
#define PERCENT_SYMBOL_X   (LCD_WIDTH - 16)  // %符号X位置（顶住右边框）

// 地址行（单独一行）
#define ADDR_LABEL_WIDTH   54  // "AD:"标签宽度
#define ADDR_VALUE_X       (UI_LEFT_MARGIN + ADDR_LABEL_WIDTH)
#define ADDR_VALUE_WIDTH   60

// 状态行
#define STATUS_VALUE_X     (UI_LEFT_MARGIN + LABEL_WIDTH)
#define STATUS_WIDTH       (LCD_WIDTH - STATUS_VALUE_X - 4)  // 状态文字占满剩余宽度

// 进度条（竖状，显示在右侧，底部对齐AD标签底部）
#define PROGRESS_BAR_X     199   // 进度条左上角X（右移2像素）
#define PROGRESS_BAR_Y     (UI_Y_TEMP_HUM + FONT_ASCII_HEIGHT + 2)  // 进度条左上角Y（温湿度行下方）
#define PROGRESS_BAR_WIDTH 28    // 进度条宽度
#define PROGRESS_BAR_HEIGHT (UI_Y_ADDRESS + FONT_ASCII_HEIGHT - PROGRESS_BAR_Y - 4)   // 进度条高度（底部上提4像素）
#define PROGRESS_BAR_BORDER 2    // 进度条边框宽度

// 定义一个标记值表示"从未初始化"（使用一个不可能的地址值）
#define ERROR_MSG_UNINITIALIZED  ((const char*)1)

// 初始值常量
#define INVALID_TEMP      -999.9f
#define INVALID_INT       -1
#define INVALID_ADDR      0xFFFF

/**
 * 格式化浮点数到字符串（1位小数）
 */
static void UI_FloatToString(float value, char* buf)
{
    unsigned char i = 0;
    int intPart = (int)value;
    int fracPart;
    
    if(value < 0)
    {
        buf[i++] = '-';
        value = -value;
        intPart = -intPart;
    }
    
    // 整数部分
    if(intPart >= 100)
    {
        buf[i++] = '0' + (intPart / 100);
        intPart %= 100;
    }
    if(intPart >= DECIMAL_BASE)
    {
        buf[i++] = '0' + (intPart / DECIMAL_BASE);
        intPart %= DECIMAL_BASE;
    }
    buf[i++] = '0' + intPart;
    
    // 小数部分
    buf[i++] = '.';
    fracPart = (int)((value - (int)value) * DECIMAL_BASE);
    if(fracPart < 0) fracPart = -fracPart;
    buf[i++] = '0' + fracPart;
    
    buf[i] = '\0';
}

/**
 * 格式化整数到字符串
 */
static void UI_IntToString(int value, char* buf)
{
    unsigned char i = 0;
    unsigned char temp[MAX_DIGITS];
    unsigned char j;
    
    if(value < 0)
    {
        buf[i++] = '-';
        value = -value;
    }
    
    if(value == 0)
    {
        temp[0] = '0';
        j = 1;
    }
    else
    {
        j = 0;
        while(value > 0 && j < MAX_DIGITS)
        {
            temp[j++] = '0' + (value % DECIMAL_BASE);
            value /= DECIMAL_BASE;
        }
    }
    
    // 反转
    while(j > 0)
    {
        buf[i++] = temp[--j];
    }
    
    buf[i] = '\0';
}

/**
 * 格式化地址到字符串（固定3位，前导0）
 */
static void UI_AddrToString(unsigned int value, char* buf)
{
    buf[0] = '0' + ((value / 100) % 10);
    buf[1] = '0' + ((value / 10) % 10);
    buf[2] = '0' + (value % 10);
    buf[3] = '\0';
}

/**
 * 快速写入16位像素数据（内联优化）
 */
#define LCD_WRITE_PIXEL(pixel_color) \
    do { \
        LCM_DB = (pixel_color) >> 8; \
        _nop_(); _nop_(); \
        LCM_WR = 0; \
        _nop_(); _nop_(); \
        LCM_WR = 1; \
        LCM_DB = (pixel_color) & 0xFF; \
        _nop_(); _nop_(); \
        LCM_WR = 0; \
        _nop_(); _nop_(); \
        LCM_WR = 1; \
    } while(0)

/**
 * 绘制36x36汉字（使用通用函数）
 */
static void UI_DrawChinese36x36(unsigned int x, unsigned int y, char* text, unsigned int color, unsigned int bg)
{
    unsigned char code* font_data;
    
    font_data = (unsigned char code*)Font_Get36x36(text);
    if(font_data == NULL) return;
    
    // 使用通用绘制函数
    LCM_Draw_Char_Generic(x, y, font_data, FONT_CHINESE_WIDTH, FONT_CHINESE_HEIGHT, color, bg);
}

// ================= 数值显示位置常量 =================
// 左侧列（温度、湿度、位置、传感器）

/**
 * UI初始化
 */
void UI_Init(void)
{
    unsigned int y;
    
    // 注意：不再禁用全局中断，以避免影响Systick定时器和其他中断
    // LCD操作本身是原子的（SPI传输在中断中完成），不需要禁用全局中断
    
    // 背景
    LCM_Fill_Color(UI_BG_COLOR);
    
    // 顶部标题栏
    LCM_FillRect(0, 0, LCD_WIDTH, UI_HEADER_HEIGHT, UI_STATUS_BAR_COLOR);
    
    // 左上角显示网络状态图标（36x36）
    // 图标位置：左边距2px，垂直居中
    {
        unsigned char net_connected = NE2_IsConnected();
        if(net_connected)
        {
            // 网络已连接 - 显示绿色ON图标
            LCM_Draw_Image_RGB565(2, (UI_HEADER_HEIGHT - NET_ICON_HEIGHT) / 2, 
                                  gImage_on, NET_ICON_WIDTH, NET_ICON_HEIGHT, NET_ICON_HEADER_SIZE);
        }
        else
        {
            // 网络未连接 - 显示灰色OFF图标
            LCM_Draw_Image_RGB565(2, (UI_HEADER_HEIGHT - NET_ICON_HEIGHT) / 2, 
                                  gImage_off, NET_ICON_WIDTH, NET_ICON_HEIGHT, NET_ICON_HEADER_SIZE);
        }
    }
    
    // 显示"料位仪"三个汉字（居中，使用苹果蓝）
    UI_DrawChinese36x36(UI_TITLE_X, UI_TITLE_Y, "料", UI_TITLE_TEXT, UI_STATUS_BAR_COLOR);
    UI_DrawChinese36x36(UI_TITLE_X + FONT_CHINESE_WIDTH, UI_TITLE_Y, "位", UI_TITLE_TEXT, UI_STATUS_BAR_COLOR);
    UI_DrawChinese36x36(UI_TITLE_X + FONT_CHINESE_WIDTH * 2, UI_TITLE_Y, "仪", UI_TITLE_TEXT, UI_STATUS_BAR_COLOR);
    
    // 顶部分隔线
    LCM_FillRect(0, UI_HEADER_HEIGHT, LCD_WIDTH, UI_SEPARATOR_HEIGHT, UI_LINE_COLOR);
    
    // === 逐行绘制背景和标签 ===
    
    // 传感器数据区（温湿度、PS）
    for(y = UI_Y_TEMP_HUM; y < UI_Y_TEMP_HUM + UI_ROW_HEIGHT * 2; y += UI_ROW_HEIGHT)
    {
        LCM_FillRect(0, y, LCD_WIDTH, UI_ROW_HEIGHT, UI_SENSOR_BG);
    }
    // 第一行不显示标签（直接显示"xxx摄氏度 xxx%湿度"）
    LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_POSITION, "PS:", UI_SENSOR_LABEL, UI_SENSOR_BG);
    
    // 系统状态区（ST, P, S, AD）- 统一底色
    for(y = UI_Y_STATUS; y < LCD_HEIGHT && y < UI_Y_ADDRESS + UI_ROW_HEIGHT; y += UI_ROW_HEIGHT)
    {
        LCM_FillRect(0, y, LCD_WIDTH, UI_ROW_HEIGHT, UI_STATUS_BG);
    }
    LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_STATUS, "ST:", UI_STATUS_LABEL, UI_STATUS_BG);
    LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_ENDPOINT, "P:", UI_STATUS_LABEL, UI_STATUS_BG);
    LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_SENSOR, "S:", UI_STATUS_LABEL, UI_STATUS_BG);
    LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_ADDRESS, "AD:", UI_STATUS_LABEL, UI_STATUS_BG);
    
    // 初始显示Normal状态
    LCM_Draw_String_16x32(STATUS_VALUE_X, UI_Y_STATUS, "Normal", UI_STATUS_NORMAL, UI_STATUS_BG);
    
    // === 分隔线（在背景之后绘制，确保不被遮挡）===
    // 分段绘制分隔线，避开进度条
    LCM_FillRect(0, UI_Y_STATUS - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
    LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_STATUS - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    LCM_FillRect(0, UI_Y_ENDPOINT - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
    LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_ENDPOINT - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    LCM_FillRect(0, UI_Y_SENSOR - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
    LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_SENSOR - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
}

// UI更新的静态缓存变量（文件作用域，供UI_ResetCache使用）
static float ui_last_temp = INVALID_TEMP;
static int ui_last_hum = INVALID_INT;
static int ui_last_pos = INVALID_INT;        // PS行缓存（激光测距数据）
static int ui_last_endpoint = INVALID_INT;   // P行缓存（终点值）
static int ui_last_percent = INVALID_INT;    // 料位百分比缓存
static int ui_last_sensor = INVALID_INT;     // S行缓存（起始位置）
static unsigned int ui_last_addr = INVALID_ADDR;
static const char* ui_last_error_msg = ERROR_MSG_UNINITIALIZED;
static unsigned char ui_last_net_status = 2; // 网络状态缓存（2=未初始化）

// 终点位置值（由app.c通过UI_SetEndpoint设置）
static int ui_endpoint_value = 1000;  // 默认1000mm

/**
 * UI更新
 */
void UI_Update(
    float temperature,
    int humidity,
    int position_mm,
    int level_percent,
    int sensor_mm,
    unsigned int addr,
    const char* error_msg
)
{
    char buf[UI_BUFFER_SIZE];
    
    // 注意：不再禁用全局中断，以避免影响Systick定时器和其他中断
    // LCD操作本身是原子的（SPI传输在中断中完成），不需要禁用全局中断
    
    // === 传感器数据区（使用传感器配色）===
    // 第一行：温度和湿度（格式："xxx摄氏度 xxx%湿度"）
    if(temperature != ui_last_temp || humidity != ui_last_hum)
    {
        unsigned char temp_len;
        unsigned char hum_len;
        unsigned int hum_x;  // 湿度显示起始X位置
        
        // 清除整行（左侧部分，不包括右侧百分比区域）
        LCM_FillRect(UI_LEFT_MARGIN, UI_Y_TEMP_HUM, PERCENT_X - UI_LEFT_MARGIN, FONT_ASCII_HEIGHT, UI_SENSOR_BG);
        
        // 显示温度："xxx摄氏度"
        UI_FloatToString(temperature, buf);
        LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_TEMP_HUM, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 计算温度数值的长度
        temp_len = 0;
        while(buf[temp_len] != '\0') temp_len++;
        
        // 显示"摄氏度"（℃符号，增加7像素间距避免重叠）
        LCM_Draw_Char_Generic(UI_LEFT_MARGIN + temp_len * 16 + 7, UI_Y_TEMP_HUM, Font_Get32x32_Symbol("℃"), FONT_SYMBOL_WIDTH, FONT_SYMBOL_HEIGHT, UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 显示湿度："xxx%"（在温度后面，留一些间距）
        hum_x = UI_LEFT_MARGIN + temp_len * 16 + 7 + FONT_SYMBOL_WIDTH + 8;
        UI_IntToString(humidity, buf);
        LCM_Draw_String_16x32(hum_x, UI_Y_TEMP_HUM, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 计算湿度数值的长度
        hum_len = 0;
        while(buf[hum_len] != '\0') hum_len++;
        
        // 显示"%"（增加4像素间距，避免拥挤）
        LCM_Draw_String_16x32(hum_x + hum_len * 16 + 4, UI_Y_TEMP_HUM, "%", UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        ui_last_temp = temperature;
        ui_last_hum = humidity;
    }
    
    // 百分比（显示在右侧，%符号顶住右边框，数字右对齐）
    if(level_percent != ui_last_percent)
    {
        unsigned char num_x;  // 数字起始X位置（右对齐）
        unsigned char len;    // 数字长度
        unsigned char fill_height;  // 进度条填充高度
        unsigned char empty_height; // 进度条空白高度
        
        LCM_FillRect(PERCENT_X, UI_Y_TEMP_HUM, LCD_WIDTH - PERCENT_X, FONT_ASCII_HEIGHT, UI_SENSOR_BG);
        UI_IntToString(level_percent, buf);
        
        // 计算数字长度并右对齐（16x32字体，每个字符16像素宽）
        len = 0;
        while(buf[len] != '\0') len++;
        
        // 右对齐：%符号前留一些间距（4像素），数字从右往左排列
        num_x = PERCENT_SYMBOL_X - 4 - (len * 16);
        
        LCM_Draw_String_16x32(num_x, UI_Y_TEMP_HUM, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        LCM_Draw_String_16x32(PERCENT_SYMBOL_X, UI_Y_TEMP_HUM, "%", UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 绘制竖状进度条（从下往上填充）
        // 1. 绘制外框（较粗边框）
        LCM_FillRect(PROGRESS_BAR_X, PROGRESS_BAR_Y, PROGRESS_BAR_WIDTH, PROGRESS_BAR_HEIGHT, UI_SENSOR_VALUE);
        
        // 2. 绘制内部背景（清空中间区域）
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_BORDER, 
                     PROGRESS_BAR_Y + PROGRESS_BAR_BORDER, 
                     PROGRESS_BAR_WIDTH - PROGRESS_BAR_BORDER * 2, 
                     PROGRESS_BAR_HEIGHT - PROGRESS_BAR_BORDER * 2, 
                     UI_SENSOR_BG);
        
        // 3. 根据百分比从下往上填充（0-100%）
        fill_height = ((unsigned long)(PROGRESS_BAR_HEIGHT - PROGRESS_BAR_BORDER * 2) * level_percent) / 100;
        empty_height = (PROGRESS_BAR_HEIGHT - PROGRESS_BAR_BORDER * 2) - fill_height;
        
        if(fill_height > 0)
        {
            LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_BORDER, 
                         PROGRESS_BAR_Y + PROGRESS_BAR_BORDER + empty_height, 
                         PROGRESS_BAR_WIDTH - PROGRESS_BAR_BORDER * 2, 
                         fill_height, 
                         UI_STATUS_NORMAL);  // 使用绿色填充
        }
        
        ui_last_percent = level_percent;
    }
    
    // 位置PS（第二行）- 显示激光测距数值(position_mm)
    // 注意：清除高度限制为31px，避免覆盖下方分隔线
    if(position_mm != ui_last_pos)
    {
        LCM_FillRect(VALUE_X, UI_Y_POSITION, VALUE_WIDTH, 31, UI_SENSOR_BG);
        UI_IntToString(position_mm, buf);
        LCM_Draw_String_16x32(VALUE_X, UI_Y_POSITION, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        LCM_Draw_String_16x32(UNIT_X, UI_Y_POSITION, "mm", UI_SENSOR_VALUE, UI_SENSOR_BG);
        ui_last_pos = position_mm;
        
        // 重绘分隔线，防止被覆盖（分段绘制，避开进度条）
        LCM_FillRect(0, UI_Y_STATUS - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_STATUS - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    }
    
    // === 状态行（ST - 第三行）===
    // 强制第一次更新或检测到变化时更新
    if(ui_last_error_msg == ERROR_MSG_UNINITIALIZED || error_msg != ui_last_error_msg)
    {
        // 分段清除状态行背景，避开进度条区域
        LCM_FillRect(STATUS_VALUE_X, UI_Y_STATUS, PROGRESS_BAR_X - STATUS_VALUE_X, FONT_ASCII_HEIGHT, UI_STATUS_BG);
        
        if(error_msg == NULL || error_msg[0] == '\0')
        {
            // 正常状态
            LCM_Draw_String_16x32(STATUS_VALUE_X, UI_Y_STATUS, "Normal", UI_STATUS_NORMAL, UI_STATUS_BG);
        }
        else
        {
            // 错误状态
            LCM_Draw_String_16x32(STATUS_VALUE_X, UI_Y_STATUS, error_msg, UI_STATUS_ERROR, UI_STATUS_BG);
        }
        ui_last_error_msg = error_msg;
    }
    
    // === 系统状态区（P, S, AD - 使用统一的状态区配色）===
    // 终点值P（第四行）- 显示终点值(ui_endpoint_value，默认1000mm)
    if(ui_endpoint_value != ui_last_endpoint)
    {
        LCM_FillRect(VALUE_X, UI_Y_ENDPOINT, VALUE_WIDTH, 31, UI_STATUS_BG);
        UI_IntToString(ui_endpoint_value, buf);
        LCM_Draw_String_16x32(VALUE_X, UI_Y_ENDPOINT, buf, UI_STATUS_VALUE, UI_STATUS_BG);
        LCM_Draw_String_16x32(UNIT_X, UI_Y_ENDPOINT, "mm", UI_STATUS_VALUE, UI_STATUS_BG);
        ui_last_endpoint = ui_endpoint_value;
        
        // 重绘分隔线，防止被覆盖（分段绘制，避开进度条）
        LCM_FillRect(0, UI_Y_ENDPOINT - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_ENDPOINT - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    }
    
    // 起始位置S（第五行）- 显示起始位置(sensor_mm)
    if(sensor_mm != ui_last_sensor)
    {
        LCM_FillRect(VALUE_X, UI_Y_SENSOR, VALUE_WIDTH, 31, UI_STATUS_BG);
        UI_IntToString(sensor_mm, buf);
        LCM_Draw_String_16x32(VALUE_X, UI_Y_SENSOR, buf, UI_STATUS_VALUE, UI_STATUS_BG);
        LCM_Draw_String_16x32(UNIT_X, UI_Y_SENSOR, "mm", UI_STATUS_VALUE, UI_STATUS_BG);
        ui_last_sensor = sensor_mm;
        
        // 重绘分隔线，防止被覆盖（分段绘制，避开进度条）
        LCM_FillRect(0, UI_Y_SENSOR - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_SENSOR - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    }
    
    // 地址（固定3位显示）
    if(addr != ui_last_addr)
    {
        LCM_FillRect(ADDR_VALUE_X, UI_Y_ADDRESS, ADDR_VALUE_WIDTH, FONT_ASCII_HEIGHT, UI_STATUS_BG);
        UI_AddrToString(addr, buf);
        LCM_Draw_String_16x32(ADDR_VALUE_X, UI_Y_ADDRESS, buf, UI_STATUS_VALUE, UI_STATUS_BG);
        ui_last_addr = addr;
    }
    
    // 网络状态图标更新（在顶部标题栏左上角）
    {
        unsigned char net_status = NE2_IsConnected();
        if(net_status != ui_last_net_status)
        {
            // 网络状态变化 - 更新图标
            if(net_status)
            {
                // 网络已连接 - 显示绿色ON图标
                LCM_Draw_Image_RGB565(2, (UI_HEADER_HEIGHT - NET_ICON_HEIGHT) / 2, 
                                      gImage_on, NET_ICON_WIDTH, NET_ICON_HEIGHT, NET_ICON_HEADER_SIZE);
            }
            else
            {
                // 网络未连接 - 显示灰色OFF图标
                LCM_Draw_Image_RGB565(2, (UI_HEADER_HEIGHT - NET_ICON_HEIGHT) / 2, 
                                      gImage_off, NET_ICON_WIDTH, NET_ICON_HEIGHT, NET_ICON_HEADER_SIZE);
            }
            
            ui_last_net_status = net_status;
        }
    }
    
    // 避免未使用参数警告
    level_percent = level_percent;
}

/**
 * UI更新（扩展版本，支持设置状态显示）
 * 当start_setting或end_setting或addr_setting为1时，对应参数以红色显示
 */
void UI_Update_Ex(
    float temperature,
    int humidity,
    int position_mm,
    int level_percent,
    int sensor_mm,
    unsigned int addr,
    const char* error_msg,
    unsigned char start_setting,  // 起点设置状态（0=正常，1=设置中红色显示）
    unsigned char end_setting,    // 终点设置状态（0=正常，1=设置中红色显示）
    unsigned char addr_setting    // 地址设置状态（0=正常，1=设置中红色显示）
)
{
    char buf[UI_BUFFER_SIZE];
    unsigned int sensor_color;  // 传感器高度（起点）显示颜色
    unsigned int pos_color;     // 位置（终点）显示颜色
    unsigned int addr_color;    // 地址显示颜色
    
    // 注意：不再禁用全局中断，以避免影响Systick定时器和其他中断
    // LCD操作本身是原子的（SPI传输在中断中完成），不需要禁用全局中断
    
    // === 传感器数据区（使用传感器配色）===
    // 第一行：温度和湿度（格式："xxx摄氏度 xxx%湿度"）
    if(temperature != ui_last_temp || humidity != ui_last_hum)
    {
        unsigned char temp_len;
        unsigned char hum_len;
        unsigned int hum_x;  // 湿度显示起始X位置
        
        // 清除整行（左侧部分，不包括右侧百分比区域）
        LCM_FillRect(UI_LEFT_MARGIN, UI_Y_TEMP_HUM, PERCENT_X - UI_LEFT_MARGIN, FONT_ASCII_HEIGHT, UI_SENSOR_BG);
        
        // 显示温度："xxx摄氏度"
        UI_FloatToString(temperature, buf);
        LCM_Draw_String_16x32(UI_LEFT_MARGIN, UI_Y_TEMP_HUM, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 计算温度数值的长度
        temp_len = 0;
        while(buf[temp_len] != '\0') temp_len++;
        
        // 显示"摄氏度"（℃符号，增加7像素间距避免重叠）
        LCM_Draw_Char_Generic(UI_LEFT_MARGIN + temp_len * 16 + 7, UI_Y_TEMP_HUM, Font_Get32x32_Symbol("℃"), FONT_SYMBOL_WIDTH, FONT_SYMBOL_HEIGHT, UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 显示湿度："xxx%"（在温度后面，留一些间距）
        hum_x = UI_LEFT_MARGIN + temp_len * 16 + 7 + FONT_SYMBOL_WIDTH + 8;
        UI_IntToString(humidity, buf);
        LCM_Draw_String_16x32(hum_x, UI_Y_TEMP_HUM, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 计算湿度数值的长度
        hum_len = 0;
        while(buf[hum_len] != '\0') hum_len++;
        
        // 显示"%"（增加4像素间距，避免拥挤）
        LCM_Draw_String_16x32(hum_x + hum_len * 16 + 4, UI_Y_TEMP_HUM, "%", UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        ui_last_temp = temperature;
        ui_last_hum = humidity;
    }
    
    // 百分比（显示在右侧，%符号顶住右边框，数字右对齐）
    if(level_percent != ui_last_percent)
    {
        unsigned char num_x;  // 数字起始X位置（右对齐）
        unsigned char len;    // 数字长度
        unsigned char fill_height;  // 进度条填充高度
        unsigned char empty_height; // 进度条空白高度
        
        LCM_FillRect(PERCENT_X, UI_Y_TEMP_HUM, LCD_WIDTH - PERCENT_X, FONT_ASCII_HEIGHT, UI_SENSOR_BG);
        UI_IntToString(level_percent, buf);
        
        // 计算数字长度并右对齐（16x32字体，每个字符16像素宽）
        len = 0;
        while(buf[len] != '\0') len++;
        
        // 右对齐：%符号前留一些间距（4像素），数字从右往左排列
        num_x = PERCENT_SYMBOL_X - 4 - (len * 16);
        
        LCM_Draw_String_16x32(num_x, UI_Y_TEMP_HUM, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        LCM_Draw_String_16x32(PERCENT_SYMBOL_X, UI_Y_TEMP_HUM, "%", UI_SENSOR_VALUE, UI_SENSOR_BG);
        
        // 绘制竖状进度条（从下往上填充）
        // 1. 绘制外框（较粗边框）
        LCM_FillRect(PROGRESS_BAR_X, PROGRESS_BAR_Y, PROGRESS_BAR_WIDTH, PROGRESS_BAR_HEIGHT, UI_SENSOR_VALUE);
        
        // 2. 绘制内部背景（清空中间区域）
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_BORDER, 
                     PROGRESS_BAR_Y + PROGRESS_BAR_BORDER, 
                     PROGRESS_BAR_WIDTH - PROGRESS_BAR_BORDER * 2, 
                     PROGRESS_BAR_HEIGHT - PROGRESS_BAR_BORDER * 2, 
                     UI_SENSOR_BG);
        
        // 3. 根据百分比从下往上填充（0-100%）
        fill_height = ((unsigned long)(PROGRESS_BAR_HEIGHT - PROGRESS_BAR_BORDER * 2) * level_percent) / 100;
        empty_height = (PROGRESS_BAR_HEIGHT - PROGRESS_BAR_BORDER * 2) - fill_height;
        
        if(fill_height > 0)
        {
            LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_BORDER, 
                         PROGRESS_BAR_Y + PROGRESS_BAR_BORDER + empty_height, 
                         PROGRESS_BAR_WIDTH - PROGRESS_BAR_BORDER * 2, 
                         fill_height, 
                         UI_STATUS_NORMAL);  // 使用绿色填充
        }
        
        ui_last_percent = level_percent;
    }
    
    // 位置PS（第二行）- 显示激光测距数值
    // position_mm 始终是激光测距位置（display_laser）
    // 注意：清除高度限制为31px，避免覆盖下方分隔线
    if(position_mm != ui_last_pos)
    {
        LCM_FillRect(VALUE_X, UI_Y_POSITION, VALUE_WIDTH, 31, UI_SENSOR_BG);
        UI_IntToString(position_mm, buf);
        LCM_Draw_String_16x32(VALUE_X, UI_Y_POSITION, buf, UI_SENSOR_VALUE, UI_SENSOR_BG);
        LCM_Draw_String_16x32(UNIT_X, UI_Y_POSITION, "mm", UI_SENSOR_VALUE, UI_SENSOR_BG);
        ui_last_pos = position_mm;
        
        // 重绘分隔线，防止被覆盖（分段绘制，避开进度条）
        LCM_FillRect(0, UI_Y_STATUS - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_STATUS - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    }
    
    // === 状态行（ST - 第三行）===
    // 强制第一次更新或检测到变化时更新
    if(ui_last_error_msg == ERROR_MSG_UNINITIALIZED || error_msg != ui_last_error_msg)
    {
        // 分段清除状态行背景，避开进度条区域
        LCM_FillRect(STATUS_VALUE_X, UI_Y_STATUS, PROGRESS_BAR_X - STATUS_VALUE_X, FONT_ASCII_HEIGHT, UI_STATUS_BG);
        
        if(error_msg == NULL || error_msg[0] == '\0')
        {
            // 正常状态
            LCM_Draw_String_16x32(STATUS_VALUE_X, UI_Y_STATUS, "Normal", UI_STATUS_NORMAL, UI_STATUS_BG);
        }
        else
        {
            // 错误状态
            LCM_Draw_String_16x32(STATUS_VALUE_X, UI_Y_STATUS, error_msg, UI_STATUS_ERROR, UI_STATUS_BG);
        }
        ui_last_error_msg = error_msg;
    }
    
    // === 系统状态区（P, S, AD - 使用统一的状态区配色）===
    // 终点值P（第四行）- 显示终点值（通过UI_SetEndpoint设置），编辑模式下以红色显示
    pos_color = end_setting ? RGB565_RED : UI_STATUS_VALUE;
    
    // 强制刷新：如果在设置状态或值变化
    if(ui_endpoint_value != ui_last_endpoint || end_setting)
    {
        LCM_FillRect(VALUE_X, UI_Y_ENDPOINT, VALUE_WIDTH, 31, UI_STATUS_BG);
        UI_IntToString(ui_endpoint_value, buf);  // 显示ui_endpoint_value（通过UI_SetEndpoint设置）
        LCM_Draw_String_16x32(VALUE_X, UI_Y_ENDPOINT, buf, pos_color, UI_STATUS_BG);
        LCM_Draw_String_16x32(UNIT_X, UI_Y_ENDPOINT, "mm", pos_color, UI_STATUS_BG);
        
        // 只在非设置状态时更新缓存
        if(!end_setting)
        {
            ui_last_endpoint = ui_endpoint_value;
        }
        
        // 重绘分隔线，防止被覆盖（分段绘制，避开进度条）
        LCM_FillRect(0, UI_Y_ENDPOINT - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_ENDPOINT - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    }
    
    // 起始位置S（第五行）- 显示起始位置(sensor_mm)，编辑模式下以红色显示
    sensor_color = start_setting ? RGB565_RED : UI_STATUS_VALUE;
    // 强制刷新：如果在设置状态或值变化
    if(sensor_mm != ui_last_sensor || start_setting)
    {
        LCM_FillRect(VALUE_X, UI_Y_SENSOR, VALUE_WIDTH, 31, UI_STATUS_BG);
        UI_IntToString(sensor_mm, buf);
        LCM_Draw_String_16x32(VALUE_X, UI_Y_SENSOR, buf, sensor_color, UI_STATUS_BG);
        LCM_Draw_String_16x32(UNIT_X, UI_Y_SENSOR, "mm", sensor_color, UI_STATUS_BG);
        
        // 只在非设置状态时更新缓存
        if(!start_setting)
        {
            ui_last_sensor = sensor_mm;
        }
        
        // 重绘分隔线，防止被覆盖（分段绘制，避开进度条）
        LCM_FillRect(0, UI_Y_SENSOR - 1, PROGRESS_BAR_X, 1, UI_LINE_COLOR);
        LCM_FillRect(PROGRESS_BAR_X + PROGRESS_BAR_WIDTH, UI_Y_SENSOR - 1, LCD_WIDTH - (PROGRESS_BAR_X + PROGRESS_BAR_WIDTH), 1, UI_LINE_COLOR);
    }
    
    // 地址（固定3位显示）- 如果在地址设置状态，以红色显示
    addr_color = addr_setting ? RGB565_RED : UI_STATUS_VALUE;
    // 强制刷新：如果在设置状态或值变化
    if(addr != ui_last_addr || addr_setting)
    {
        LCM_FillRect(ADDR_VALUE_X, UI_Y_ADDRESS, ADDR_VALUE_WIDTH, FONT_ASCII_HEIGHT, UI_STATUS_BG);
        UI_AddrToString(addr, buf);
        LCM_Draw_String_16x32(ADDR_VALUE_X, UI_Y_ADDRESS, buf, addr_color, UI_STATUS_BG);
        
        // 只在非设置状态时更新缓存
        if(!addr_setting)
        {
            ui_last_addr = addr;
        }
    }
    
    // 网络状态图标更新（在顶部标题栏左上角）
    {
        unsigned char net_status = NE2_IsConnected();
        if(net_status != ui_last_net_status)
        {
            // 网络状态变化 - 更新图标
            if(net_status)
            {
                // 网络已连接 - 显示绿色ON图标
                LCM_Draw_Image_RGB565(2, (UI_HEADER_HEIGHT - NET_ICON_HEIGHT) / 2, 
                                      gImage_on, NET_ICON_WIDTH, NET_ICON_HEIGHT, NET_ICON_HEADER_SIZE);
            }
            else
            {
                // 网络未连接 - 显示灰色OFF图标
                LCM_Draw_Image_RGB565(2, (UI_HEADER_HEIGHT - NET_ICON_HEIGHT) / 2, 
                                      gImage_off, NET_ICON_WIDTH, NET_ICON_HEIGHT, NET_ICON_HEADER_SIZE);
            }
            
            ui_last_net_status = net_status;
        }
    }
    
    // 避免未使用参数警告
    level_percent = level_percent;
}

/**
 * 重置UI更新缓存
 * 用于页面切换或唤醒后强制刷新所有UI元素
 */
void UI_ResetCache(void)
{
    ui_last_temp = INVALID_TEMP;
    ui_last_hum = INVALID_INT;
    ui_last_pos = INVALID_INT;
    ui_last_endpoint = INVALID_INT;
    ui_last_percent = INVALID_INT;
    ui_last_sensor = INVALID_INT;
    ui_last_addr = INVALID_ADDR;
    ui_last_error_msg = ERROR_MSG_UNINITIALIZED;
    ui_last_net_status = 2;  // 重置网络状态缓存
    // 注意：ui_endpoint_value 保持不变，不重置
}

/**
 * 设置终点值（用于从EEPROM/Modbus读取的终点值）
 * 在启动或读取配置后调用，确保UI显示正确的终点值
 */
void UI_SetEndpoint(int endpoint_mm)
{
    // 只有当值真正变化时才强制刷新，避免不必要的重绘
    if(ui_endpoint_value != endpoint_mm)
    {
        ui_endpoint_value = endpoint_mm;
        ui_last_endpoint = INVALID_INT;  // 强制刷新显示
    }
}

/**
 * 清屏
 */
void UI_Clear(void)
{
    LCM_Fill_Color(RGB565_WHITE);
}
