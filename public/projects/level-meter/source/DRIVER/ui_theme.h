#ifndef __UI_THEME_H__
#define __UI_THEME_H__

#include "st7789.h"

// ===== Theme Colors (RGB565) =====
// 深色工业风格配色方案

// 背景色
#define UI_BG_COLOR         RGB565(255,0,0)      // 红色背景
#define UI_STATUS_BAR_COLOR RGB565(28,28,28)     // 状态栏背景（稍亮）
#define UI_PANEL_COLOR      RGB565(26,26,26)     // 数据面板背景（暗底板块）

// 卡片和容器
#define UI_CARD_COLOR       RGB565(40,40,40)     // 信息卡片背景
#define UI_BORDER_COLOR     RGB565(70,70,70)     // 边框颜色
#define UI_DIVIDER_COLOR    RGB565(50,50,50)     // 分割线颜色

// 文字颜色
#define UI_TEXT_MAIN        RGB565(230,230,230)   // 主文字（白色）
#define UI_TEXT_SUB         RGB565(160,160,160)   // 次要文字（浅灰）
#define UI_TEXT_DIM         RGB565(100,100,100)   // 暗淡文字（深灰）
#define UI_TEXT_LABEL       RGB565(150,150,150)   // 标签文字（浅灰）
#define UI_TEXT_VALUE       RGB565(235,235,235)   // 数值文字（亮色）
#define UI_LINE_COLOR       RGB565(90,90,90)      // 分隔线颜色（更明显）

// 强调色
#define UI_ACCENT           RGB565(0,180,255)    // 强调色（蓝色）
#define UI_ACCENT_DARK      RGB565(0,120,180)    // 强调色深色
#define UI_SUCCESS          RGB565(0,200,100)     // 成功（绿色）
#define UI_WARN             RGB565(255,150,0)     // 警告（橙色）
#define UI_ERROR            RGB565(255,80,80)     // 错误（红色）

// 按钮状态
#define UI_BTN_NORMAL       UI_CARD_COLOR
#define UI_BTN_ACTIVE       UI_ACCENT
#define UI_BTN_TEXT_NORMAL  UI_TEXT_MAIN
#define UI_BTN_TEXT_ACTIVE  RGB565(0,0,0)        // 激活按钮文字（黑色）

#endif /* __UI_THEME_H__ */

