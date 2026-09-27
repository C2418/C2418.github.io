# UI界面系统使用说明

## 文件结构

```
DRIVER/
├── ui.h              // UI接口定义
├── ui.c              // UI实现
├── ui_theme.h        // 颜色主题定义
├── st7789.h          // LCD驱动（已更新，新增FillRect和DrawRect函数）
└── st7789.c          // LCD驱动实现
```

## 快速开始

### 1. 基本初始化

```c
#include "ui.h"
#include "st7789.h"

void main(void)
{
    // 初始化LCD
    LCM_Init();
    
    // 初始化UI系统
    UI_Init();
    
    while(1)
    {
        // 你的主循环
    }
}
```

### 2. 绘制主页面

```c
// 在App_ProcessLCD()函数中使用
void App_ProcessLCD(void)
{
    static unsigned long last_update = 0;
    
    // 每500ms更新一次（可根据需要调整）
    if(Systick_Elapsed(last_update, 500))
    {
        // 从MODBUS寄存器读取数据
        unsigned short temp_reg = Modbus_GetHoldingReg(3001);
        unsigned short hum_reg = Modbus_GetHoldingReg(3002);
        unsigned char auto_mode = Modbus_GetHoldingReg(4001) & 0x01;
        
        // 转换为浮点数
        float temp = (float)((temp_reg >> 8) & 0x7F) + (float)(temp_reg & 0x0F) * 0.1f;
        int hum = (hum_reg >> 8) & 0xFF;
        
        // 绘制主页面
        UI_DrawMainPage(temp, hum, auto_mode);
        
        last_update = Systick_GetTick();
    }
}
```

## API说明

### UI_Init()
初始化UI系统，清屏并绘制状态栏。

### UI_DrawMainPage(float temp, int hum, unsigned char autoMode)
绘制主页面，包含：
- 温度卡片
- 湿度卡片
- 模式按钮（AUTO/MANUAL）

参数：
- `temp`: 温度值（浮点数）
- `hum`: 湿度值（整数，0-100）
- `autoMode`: 自动模式标志（1=自动，0=手动）

### UI_DrawStatusBar(const char *title, const char *timeStr)
绘制顶部状态栏。

参数：
- `title`: 左侧标题文字
- `timeStr`: 右侧时间文字（可传NULL）

### UI_DrawInfoCard(int x, int y, int w, int h, const char *title, const char *value, const char *unit)
绘制信息卡片。

参数：
- `x, y`: 卡片左上角坐标
- `w, h`: 卡片宽度和高度
- `title`: 卡片标题
- `value`: 数值字符串
- `unit`: 单位字符串（可传NULL）

### UI_DrawButton(int x, int y, int w, int h, const char *text, unsigned char active)
绘制按钮。

参数：
- `x, y`: 按钮左上角坐标
- `w, h`: 按钮宽度和高度
- `text`: 按钮文字
- `active`: 激活状态（1=激活，0=未激活）

### UI_Clear()
清屏，填充背景色。

## 颜色主题

所有颜色定义在 `ui_theme.h` 中，可以根据需要修改：

```c
#define UI_BG_COLOR         RGB565(18,18,18)     // 深灰背景
#define UI_CARD_COLOR       RGB565(40,40,40)     // 信息卡片
#define UI_TEXT_MAIN        RGB565(230,230,230)  // 主文字
#define UI_ACCENT           RGB565(0,180,255)    // 强调色（蓝色）
```

## 集成到现有代码

### 方案1：完全替换（推荐）

在 `app.c` 的 `App_ProcessLCD()` 函数中，替换现有的显示代码：

```c
void App_ProcessLCD(void)
{
    static unsigned long last_update = 0;
    static unsigned char ui_initialized = 0;
    
    // 初始化UI（仅一次）
    if(ui_initialized == 0)
    {
        // LCD硬件初始化
        P3M0 |= 0x10;
        P3M1 &= ~0x10;
        LCM_PWM = 1;
        LCM_Init();
        Delay_ms(100);
        
        // UI初始化
        UI_Init();
        ui_initialized = 1;
    }
    
    // 更新显示（每500ms）
    if(Systick_Elapsed(last_update, 500))
    {
        // 读取数据
        unsigned short temp_reg = Modbus_GetHoldingReg(3001);
        unsigned short hum_reg = Modbus_GetHoldingReg(3002);
        unsigned char auto_mode = Modbus_GetHoldingReg(4001) & 0x01;
        
        // 转换数据格式
        float temp = (float)((temp_reg >> 8) & 0x7F) + (float)(temp_reg & 0x0F) * 0.1f;
        int hum = (hum_reg >> 8) & 0xFF;
        
        // 绘制UI
        UI_DrawMainPage(temp, hum, auto_mode);
        
        last_update = Systick_GetTick();
    }
}
```

### 方案2：渐进式集成

保留现有的显示代码，逐步替换为UI函数调用。

## 注意事项

1. **内存使用**：UI系统使用标准C库的sprintf函数，确保已包含stdio.h
2. **刷新频率**：建议500ms-1000ms刷新一次，避免过于频繁
3. **非阻塞**：UI绘制函数是阻塞的，建议在非关键路径调用
4. **屏幕尺寸**：当前适配240x240，如需其他尺寸需调整坐标

## 扩展功能

### 添加更多信息卡片

```c
// 在UI_DrawMainPage中添加
char level_str[16];
sprintf(level_str, "%d", level_value);
UI_DrawInfoCard(10, 200, 220, 50, "LEVEL", level_str, "mm");
```

### 自定义颜色

```c
// 在ui_theme.h中修改颜色定义
#define UI_ACCENT RGB565(255, 100, 0)  // 改为橙色
```

## 效果预览

新的UI系统提供：
- ✅ 深色工业风格界面
- ✅ 清晰的信息卡片布局
- ✅ 突出的数值显示
- ✅ 高亮的模式按钮
- ✅ 专业的视觉效果

