/**
 * @file encoder.h
 * @brief 编码器控制模块
 */

#ifndef ENCODER_H
#define ENCODER_H

#include "tuya_cloud_types.h"

/* 编码器配置 */
#define ENABLE_ENCODER_CONTROL         1
#define ENC_A_PIN                      GPIO_NUM_15    /* 编码器 A 相 */
#define ENC_B_PIN                      GPIO_NUM_17    /* 编码器 B 相 */
#define ENC_E_PIN                      GPIO_NUM_9     /* 编码器按键（E） */
#define ENCODER_POLL_INTERVAL_MS       2              /* 主任务轮询间隔（按键检测等） */
#define ENCODER_BUTTON_DEBOUNCE_MS     30
#define ENCODER_COUNTS_PER_DETENT      4              /* 4倍解码：每个物理档位 4 次有效跳变 */
#define ENCODER_RESET_THRESHOLD_MS     5000           /* 编码器复位阈值 5秒 */
#define ENCODER_TIMER_SAMPLE_INTERVAL_US  50          /* 定时器采样间隔：50us = 20kHz */
#define ENCODER_TIMER_ID               TIMER_NUM_0   /* 使用定时器0 */

/* 编码器控制函数 */
VOID encoder_init(VOID);

/* 编码器任务启动（兼容性函数） */
VOID encoder_start_task(VOID);

#endif /* ENCODER_H */
