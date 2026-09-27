#ifndef __UI_H__
#define __UI_H__

/**
 * UI界面模块
 * 工业设备状态监控界面（信息密集但不乱）
 */

// UI初始化
void UI_Init(void);

// UI更新（显示5个数据）
void UI_Update(
    float temperature,      // 温度
    int humidity,          // 湿度
    int level_percent,     // 液位百分比
    unsigned int reg_addr, // 寄存器地址
    int level_value        // 液位当前值
);

// 清屏
void UI_Clear(void);

#endif /* __UI_H__ */
