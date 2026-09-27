/**
 * @file touch.h
 * @brief 触摸板控制模块
 * 
 * 功能：
 *  - 单击：开关灯
 *  - 双击+按住2s：切换色温模式
 *  - 长按：切换亮度（最亮/最暗）
 *  - 长按10s：复位设备
 */

#ifndef TOUCH_H
#define TOUCH_H

#include "tuya_cloud_types.h"

/* 触摸板初始化 */
VOID touch_init(VOID);

/* 触摸板任务启动 */
VOID touch_start_task(VOID);

#endif /* TOUCH_H */

