// ui.c
// 工业设备状态监控界面实现

#include "ui.h"
#include "ui_theme.h"
#include "st7789.h"
#include "font_ascii_8x16.h"
#include "Config.h"

// ================= UI布局常量 =================
// 全屏结构化布局
#define UI_STATUS_BAR_H    28   // 顶部状态栏高度
#define UI_DATA_PANEL_Y     34   // 数据面板起始Y坐标
#define UI_DATA_PANEL_H     150  // 数据面板高度
#define UI_BOTTOM_BAR_Y     188  // 底部信息栏起始Y坐标

// 统一行Y坐标（等距，每行22像素高，相对于数据面板）
#define UI_Y_TEMP      (UI_DATA_PANEL_Y + 0)   // 温度行
#define UI_Y_HUM        (UI_DATA_PANEL_Y + 22)  // 湿度行
#define UI_Y_LINE       (UI_DATA_PANEL_Y + 44)  // 中部分隔线
#define UI_Y_LEVEL_P    (UI_DATA_PANEL_Y + 48)  // 液位百分比行
#define UI_Y_LEVEL_V    (UI_DATA_PANEL_Y + 70)  // 液位值行
#define UI_Y_ADDR       (UI_DATA_PANEL_Y + 92)  // 地址行

// 字符宽度常量（2倍放大后：16像素宽，18像素间距）
#define UI_CHAR_WIDTH      16  // 字符实际宽度
#define UI_CHAR_SPACING    18  // 字符间距（包含2像素间隙）

// ================= 内部辅助函数 =================

/**
 * 绘制单行数据项（真正右对齐）
 * @param y Y坐标
 * @param label 标签文本（首字母）
 * @param value 数值文本
 * @param unit 单位文本（可为NULL）
 */
static void UI_DrawItem(unsigned int y, const char* label, const char* value, const char* unit)
{
    unsigned char valueLen, unitLen;
    unsigned int x_pos;
    
    // 清理这一行（20像素高，在数据面板上）
    LCM_FillRect(4, y, 232, 20, UI_PANEL_COLOR);
    
    // 绘制标签（左对齐，12像素左边距，在面板内）
    if(label != NULL)
    {
        LCM_Draw_String_8x16_Medium(12, y + 2, label, UI_TEXT_LABEL, UI_PANEL_COLOR);
    }
    
    // 绘制数值（真正右对齐）
    if(value != NULL)
    {
        // 手动计算数值长度
        valueLen = 0;
        while(value[valueLen] != '\0' && valueLen < 16) valueLen++;
        
        // 数值右对齐：基准线 x=200（如果有单位，则基准线是单位左侧）
        if(unit != NULL)
        {
            // 有单位：数值右对齐到单位左侧（单位从208开始，数值结束在208之前）
            unitLen = 0;
            while(unit[unitLen] != '\0' && unitLen < 8) unitLen++;
            x_pos = 208 - valueLen * UI_CHAR_SPACING;  // 单位在208，数值在单位左侧
        }
        else
        {
            // 无单位：数值右对齐到屏幕右侧（240 - 8 - valueLen * 18）
            x_pos = 240 - 8 - valueLen * UI_CHAR_SPACING;
        }
        
        // 确保不超出左边界
        if(x_pos < 60) x_pos = 60;  // 最小位置限制，避免与标签重叠
        
        LCM_Draw_String_8x16_Medium(x_pos, y + 2, value, UI_TEXT_VALUE, UI_PANEL_COLOR);
    }
    
    // 绘制单位（紧贴右侧，208像素位置）
    if(unit != NULL)
    {
        LCM_Draw_String_8x16_Medium(208, y + 2, unit, UI_TEXT_LABEL, UI_PANEL_COLOR);
    }
}

/**
 * 格式化浮点数到字符串（手动转换，避免sprintf占用XDATA）
 */
static void UI_FloatToString(float value, char* buf, unsigned char precision)
{
    unsigned char i = 0;
    int intPart = (int)value;
    int fracPart;
    
    if(value < 0)
    {
        buf[i++] = '-';
        intPart = -intPart;
    }
    
    // 转换整数部分
    if(intPart >= 100)
    {
        buf[i++] = '0' + (intPart / 100);
        intPart %= 100;
    }
    if(intPart >= 10)
    {
        buf[i++] = '0' + (intPart / 10);
        intPart %= 10;
    }
    buf[i++] = '0' + intPart;
    
    // 转换小数部分
    if(precision == 1)
    {
        buf[i++] = '.';
        fracPart = (int)((value - (int)value) * 10);
        if(fracPart < 0) fracPart = -fracPart;
        buf[i++] = '0' + fracPart;
    }
    
    buf[i] = '\0';
}

/**
 * 格式化整数到字符串（手动转换）
 */
static void UI_IntToString(int value, char* buf)
{
    unsigned char i = 0;
    unsigned char temp[16];
    unsigned char j;
    
    if(value < 0)
    {
        buf[i++] = '-';
        value = -value;
    }
    
    // 转换为字符串（反向）
    if(value == 0)
    {
        temp[0] = '0';
        j = 1;
    }
    else
    {
        j = 0;
        while(value > 0 && j < 16)
        {
            temp[j++] = '0' + (value % 10);
            value /= 10;
        }
    }
    
    // 反转字符串
    while(j > 0)
    {
        buf[i++] = temp[--j];
    }
    
    buf[i] = '\0';
}

/**
 * 格式化十六进制到字符串（0xXXXX格式）
 */
static void UI_HexToString(unsigned int value, char* buf)
{
    unsigned char i;
    unsigned char nibble;
    
    buf[0] = '0';
    buf[1] = 'x';
    
    // 转换4位十六进制
    for(i = 0; i < 4; i++)
    {
        nibble = (value >> (12 - i * 4)) & 0x0F;
        if(nibble < 10)
            buf[2 + i] = '0' + nibble;
        else
            buf[2 + i] = 'A' + (nibble - 10);
    }
    
    buf[6] = '\0';
}

// ================= 公共接口函数 =================

/**
 * UI初始化
 */
void UI_Init(void)
{
    // 整屏背景
    LCM_Fill_Color(UI_BG_COLOR);
    
    // ===== 顶部状态栏 =====
    LCM_FillRect(0, 0, 240, UI_STATUS_BAR_H, UI_STATUS_BAR_COLOR);
    // RUNING 居中显示（6字符*18像素=108，240-108=132，132/2=66）
    LCM_Draw_String_8x16_Medium(66, 6, "RUNING", UI_ACCENT, UI_STATUS_BAR_COLOR);
    
    // 顶部分隔线
    LCM_FillRect(0, UI_STATUS_BAR_H, 240, 2, UI_LINE_COLOR);
    
    // ===== 数据区背景面板（暗底板块） =====
    LCM_FillRect(4, UI_DATA_PANEL_Y, 232, UI_DATA_PANEL_H, UI_PANEL_COLOR);
    
    // 中部分隔线（在湿度行和液位行之间）
    LCM_FillRect(0, UI_Y_LINE, 240, 2, UI_LINE_COLOR);
    
    // 底部分隔线
    LCM_FillRect(0, UI_BOTTOM_BAR_Y, 240, 2, UI_LINE_COLOR);
    
    // ===== 底部信息栏 =====
    LCM_Draw_String_8x16_Medium(70, 204, "MONITOR", UI_TEXT_LABEL, UI_BG_COLOR);
}

/**
 * UI更新（显示5个数据）
 */
void UI_Update(
    float temperature,      // 温度
    int humidity,          // 湿度
    int level_percent,     // 液位百分比
    unsigned int reg_addr, // 寄存器地址
    int level_value        // 液位当前值
)
{
    char buf[16];
    unsigned char valueLen;
    unsigned int x_pos;
    
    // 温度行（标签：T）
    UI_FloatToString(temperature, buf, 1);
    UI_DrawItem(UI_Y_TEMP, "T", buf, "C");
    
    // 湿度行（标签：D）
    UI_IntToString(humidity, buf);
    UI_DrawItem(UI_Y_HUM, "D", buf, "%");
    
    // 数据分组分隔线（软分隔线，在湿度和液位之间，更高级）
    LCM_FillRect(12, UI_Y_LINE + 2, 216, 1, RGB565(80,80,80));
    
    // 液位百分比行（标签：L%，关键数据用强调色）
    // 先清理这一行，避免从100%变成小于100的值时百位"1"残留
    LCM_FillRect(4, UI_Y_LEVEL_P, 232, 20, UI_PANEL_COLOR);
    UI_IntToString(level_percent, buf);
    // 标签
    LCM_Draw_String_8x16_Medium(12, UI_Y_LEVEL_P + 2, "L%", UI_TEXT_LABEL, UI_PANEL_COLOR);
    // 数值（右对齐，用强调色）
    valueLen = 0;
    while(buf[valueLen] != '\0' && valueLen < 16) valueLen++;
    x_pos = 208 - valueLen * UI_CHAR_SPACING;
    if(x_pos < 60) x_pos = 60;
    LCM_Draw_String_8x16_Medium(x_pos, UI_Y_LEVEL_P + 2, buf, UI_ACCENT, UI_PANEL_COLOR);
    // 单位
    LCM_Draw_String_8x16_Medium(208, UI_Y_LEVEL_P + 2, "%", UI_TEXT_LABEL, UI_PANEL_COLOR);
    
    // 液位当前值行（标签：L）
    UI_IntToString(level_value, buf);
    UI_DrawItem(UI_Y_LEVEL_V, "L", buf, NULL);
    
    // 寄存器地址行（标签：A）
    UI_HexToString(reg_addr, buf);
    UI_DrawItem(UI_Y_ADDR, "A", buf, NULL);
}

/**
 * 清屏（倒计时结束后填充纯白色）
 */
void UI_Clear(void)
{
    LCM_Fill_Color(RGB565_WHITE);  // 填充纯白色
}
