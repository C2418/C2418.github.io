#ifndef __UI_H__
#define __UI_H__

/**
 * UI界面模块
 * 料位仪监控界面
 */

// ================= 屏幕尺寸常量 =================
#define LCD_WIDTH          240
#define LCD_HEIGHT         240

// ================= UI布局常量 =================
#define UI_HEADER_HEIGHT   46   // 顶部标题高度（容纳36px汉字+边距）
#define UI_DATA_START_Y    50   // 数据区起始Y坐标
#define UI_ROW_HEIGHT      32   // 每行高度（32px字符高度，紧凑布局）
#define UI_LEFT_MARGIN     2    // 左侧起始X
#define UI_RIGHT_COL_X     120  // 右侧列起始X（料位显示）

// 分隔线
#define UI_SEPARATOR_HEIGHT  2  // 分隔线高度

// 各行Y坐标常量（传感器数据 + 状态 + Modbus状态）
#define UI_Y_TEMP_HUM     (UI_DATA_START_Y + 0 * UI_ROW_HEIGHT)   // 温湿度行（第一行）
#define UI_Y_POSITION     (UI_DATA_START_Y + 1 * UI_ROW_HEIGHT)   // 位置行PS（第二行）
#define UI_Y_STATUS       (UI_DATA_START_Y + 2 * UI_ROW_HEIGHT)   // 状态行ST（第三行）
#define UI_Y_ENDPOINT     (UI_DATA_START_Y + 3 * UI_ROW_HEIGHT)   // 终点值P（第四行，新增）
#define UI_Y_SENSOR       (UI_DATA_START_Y + 4 * UI_ROW_HEIGHT)   // 传感器行S（第五行）
#define UI_Y_ADDRESS      (UI_DATA_START_Y + 5 * UI_ROW_HEIGHT)   // 地址行AD（第六行）

// 数据值X坐标偏移
#define UI_VALUE_OFFSET_X  20   // 数据值相对左侧的X偏移

// 字符串缓冲区大小
#define UI_BUFFER_SIZE     32   // 格式化字符串的缓冲区大小

// 标题文字居中计算（3个36x36汉字）
#define UI_TITLE_CHARS     3    // "料位仪"3个字
#define UI_TITLE_WIDTH     (UI_TITLE_CHARS * FONT_CHINESE_WIDTH)
#define UI_TITLE_X         ((LCD_WIDTH - UI_TITLE_WIDTH) / 2)
#define UI_TITLE_Y         5    // 标题Y坐标（增加上边距避免被遮挡）

// ================= 公共接口 =================

// UI初始化
void UI_Init(void);

// UI更新
void UI_Update(
    float temperature,      // 温度(℃)
    int humidity,          // 湿度(%)
    int position_mm,       // 位置高度(mm)
    int level_percent,     // 料位百分比(%)
    int sensor_mm,         // 传感器高度(mm)
    unsigned int addr,     // 地址
    const char* error_msg  // 错误报文（可为NULL）
);

// UI更新（扩展版本，支持设置状态显示）
void UI_Update_Ex(
    float temperature,      // 温度(℃)
    int humidity,          // 湿度(%)
    int position_mm,       // 位置高度(mm)
    int level_percent,     // 料位百分比(%)
    int sensor_mm,         // 传感器高度(mm)
    unsigned int addr,     // 地址
    const char* error_msg, // 错误报文（可为NULL）
    unsigned char start_setting,  // 起点设置状态（0=正常，1=设置中红色显示）
    unsigned char end_setting,    // 终点设置状态（0=正常，1=设置中红色显示）
    unsigned char addr_setting    // 地址设置状态（0=正常，1=设置中红色显示）
);

// 重置UI更新缓存（用于页面切换或唤醒后强制刷新）
void UI_ResetCache(void);

// 设置终点值（用于从EEPROM/Modbus读取的终点值）
void UI_SetEndpoint(int endpoint_mm);

// 清屏
void UI_Clear(void);

#endif /* __UI_H__ */
