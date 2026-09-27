/**
 * @file offline_mode.h
 * @brief 离线模式模块 - 整合编码器、触摸板、灯光控制功能
 * 
 * 功能：
 *  - 编码器控制（旋转调亮度/色温，按键开关灯）
 *  - 触摸板控制（单击开关，双击切换模式，长按调亮度）
 *  - 灯光控制（冷光/暖光/混合模式）
 */

#ifndef OFFLINE_MODE_H
#define OFFLINE_MODE_H

#include "tuya_cloud_types.h"

/* 运行模式枚举 */
typedef enum {
    RUN_MODE_OFFLINE = 0,  /* 离线模式：编码器/触摸板控制 */
    RUN_MODE_ONLINE        /* 在线模式：APP/云端控制 */
} RUN_MODE_E;

/* 离线模式控制 */
VOID offline_mode_init(VOID);
VOID offline_mode_enable(BOOL_T enable);
BOOL_T offline_mode_is_enabled(VOID);

/* 模式状态控制（只能通过代码手动切换） */
VOID set_run_mode(RUN_MODE_E mode);  /* 设置运行模式（手动切换） */
RUN_MODE_E get_run_mode(VOID);       /* 获取当前运行模式 */

/* 断电复位相关（内部函数，供其他模块调用） */
VOID __offline_notify_provisioned(VOID);  /* 通知配网成功，开始计时 */

#endif /* OFFLINE_MODE_H */

