#ifndef __UI_THEME_H__
#define __UI_THEME_H__

#include "st7789.h"

// ===== Theme Colors (RGB565) =====
// 现代工业风/苹果风配色方案

// 主背景色
#define UI_BG_COLOR              RGB565(18,18,18)       // 深灰黑背景（类似苹果深色模式）
#define UI_STATUS_BAR_COLOR      RGB565(30,30,30)       // 顶部标题栏背景（稍亮）

// 传感器数据区（T, RH, PS）
#define UI_SENSOR_BG             RGB565(25,25,28)       // 传感器区域背景（略带蓝灰）
#define UI_SENSOR_LABEL          RGB565(180,180,185)    // 传感器标签（较亮灰色）
#define UI_SENSOR_VALUE          RGB565(255,255,255)    // 传感器数值（纯白）

// 状态区（ST、S、AD统一为系统状态区）
#define UI_STATUS_BG             RGB565(25,22,28)       // 系统状态区背景（略带紫灰，统一色调）
#define UI_STATUS_LABEL          RGB565(180,180,185)    // 状态标签（较亮灰色）
#define UI_STATUS_NORMAL         RGB565(52,199,89)      // 正常状态（苹果绿）
#define UI_STATUS_ERROR          RGB565(255,69,58)      // 错误状态（苹果红）
#define UI_STATUS_WARN           RGB565(255,159,10)     // 警告状态（苹果橙）
#define UI_STATUS_VALUE          RGB565(255,214,10)     // 状态数值（苹果金色）

// 顶部标题
#define UI_TITLE_TEXT            RGB565(10,132,255)     // 标题文字（苹果蓝）

// 分隔线
#define UI_LINE_COLOR            RGB565(58,58,60)       // 分隔线（苹果深灰）

// 单位文字
#define UI_UNIT_TEXT             RGB565(142,142,147)    // 单位符号（浅灰）

// 兼容旧定义（用于未分区的地方）
#define UI_TEXT_LABEL            UI_SENSOR_LABEL
#define UI_TEXT_VALUE            UI_SENSOR_VALUE

#endif /* __UI_THEME_H__ */

