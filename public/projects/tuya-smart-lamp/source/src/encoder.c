/**
 * @file encoder.c
 * @brief 编码器控制模块实现
 */

#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "tkl_gpio.h"
#include "encoder.h"
#include "offline_mode.h"  /* 用于RUN_MODE_E定义和get_run_mode函数 */

/* 类型定义（与tuya_app_main.c保持一致，C语言允许在不同编译单元中重复定义相同类型） */
typedef enum {
    LIGHT_CONTROL_PRIO_NONE = 0,
    LIGHT_CONTROL_PRIO_FADE = 1,
    LIGHT_CONTROL_PRIO_TOUCH_CLICK = 2,
    LIGHT_CONTROL_PRIO_TOUCH_LONG = 3,
    LIGHT_CONTROL_PRIO_ENCODER = 4,
} LIGHT_CONTROL_PRIO_E;

typedef UINT32_T LIGHT_CONTROL_TOKEN;
#define LIGHT_CONTROL_TOKEN_INVALID  ((LIGHT_CONTROL_TOKEN)0)

/* 外部变量和函数声明（来自tuya_app_main.c） */
extern BOOL_T s_light_power;
extern volatile UINT16_T s_current_duty_cold;
extern volatile UINT16_T s_current_duty_warm;
extern volatile UINT16_T s_fade_target_duty_cold;
extern volatile UINT16_T s_fade_target_duty_warm;
extern UINT16_T s_brightness_value;
extern UINT16_T s_color_value;
/* s_last_nonzero_brightness 是STATIC变量，无法直接访问，通过light_set_power函数间接使用 */
extern volatile RUN_MODE_E s_current_run_mode;  /* 运行模式：在线/离线 */

/* 外部函数声明 */
extern VOID light_apply_pwm_direct(UINT16_T duty_cold, UINT16_T duty_warm);
extern VOID fade_start(UINT16_T target_cold, UINT16_T target_warm, UINT32_T duration_ms);
extern VOID fade_update_target(UINT16_T target_cold, UINT16_T target_warm);
extern VOID fade_stop(VOID);
extern BOOL_T fade_is_active(VOID);
extern VOID light_compute_target_duty(UINT16_T *duty_cold, UINT16_T *duty_warm);
extern OPERATE_RET light_set_power(BOOL_T on);
extern VOID light_control_set(BOOL_T on);  /* 触摸板使用的开关灯函数 */
extern VOID upload_device_all_status(VOID);  /* 上传状态到app */
extern VOID dp_update_realtime_light_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);  /* 保存状态到KV */
extern BOOL_T light_control_network_indicator_active(VOID);  /* 检查配网指示灯是否激活 */
extern LIGHT_CONTROL_TOKEN light_control_request(UINT32_T owner_id, LIGHT_CONTROL_PRIO_E priority);
extern VOID light_control_release(LIGHT_CONTROL_TOKEN token);
extern LIGHT_CONTROL_PRIO_E light_control_get_current(VOID);

/* 适配1103的全局变量和常量（使用tuya_app_main.c中的定义） */
#define g_power_on s_light_power
#define g_light_mode LIGHT_MODE_MIXED  /* 9999应用使用混合模式 */
#ifndef LIGHT_MODE_COLD
#define LIGHT_MODE_COLD 0
#define LIGHT_MODE_WARM 1
#define LIGHT_MODE_MIXED 2
#endif

/* 常量定义（如果未定义则使用默认值） */
#ifndef MIN_DUTY
#define MIN_DUTY 8  /* 最低亮度（0.5%） */
#endif
#ifndef PWM_DUTY_MAX
#define PWM_DUTY_MAX 1500  /* 最大占空比 */
#endif
#ifndef FADE_DURATION_MS
#define FADE_DURATION_MS 3000  /* 渐变时长 */
#endif
/* BRIGHT_VALUE_MIN 已在 tuya_app_main.c 中定义，直接使用 */

/* 适配1103的变量（g_last_duty_before_off和g_last_total_brightness） */
/* 注意：s_last_nonzero_brightness是STATIC变量，无法直接访问
 * 这些宏仅用于日志输出，实际值通过light_set_power函数内部处理 */
#define g_last_duty_before_off ((s_brightness_value * PWM_DUTY_MAX) / 1000)
#define g_last_total_brightness s_brightness_value

STATIC THREAD_HANDLE s_encoder_thread = NULL;
STATIC UINT32_T s_encoder_last_rotation_time = 0;  /* 上次旋转时间，用于超时检测 */
STATIC INT32_T s_encoder_accumulated_delta = 0;   /* 累积的delta值 */
STATIC LIGHT_CONTROL_TOKEN s_encoder_token = LIGHT_CONTROL_TOKEN_INVALID;  /* 编码器控制token */
STATIC UINT32_T s_encoder_last_state_change_time = 0;  /* 上次状态改变时间，用于延迟保存上传 */
STATIC BOOL_T s_encoder_pending_save_upload = FALSE;  /* 标记是否有待保存和上传的状态 */
#define ENCODER_SAVE_UPLOAD_DELAY_MS 500  /* 旋转停止后延迟500ms再保存和上传 */

/* 读取编码器AB相状态 */
STATIC UINT8_T __encoder_read_ab(VOID)
{
    TUYA_GPIO_LEVEL_E la = TUYA_GPIO_LEVEL_HIGH;
    TUYA_GPIO_LEVEL_E lb = TUYA_GPIO_LEVEL_HIGH;

    (VOID)tkl_gpio_read(ENC_A_PIN, &la);
    (VOID)tkl_gpio_read(ENC_B_PIN, &lb);

    return (UINT8_T)((la == TUYA_GPIO_LEVEL_HIGH ? 1 : 0) |
                     ((lb == TUYA_GPIO_LEVEL_HIGH ? 1 : 0) << 1));
}

/* 读取编码器按键状态 */
STATIC BOOL_T __encoder_button_active_low(VOID)
{
    TUYA_GPIO_LEVEL_E le = TUYA_GPIO_LEVEL_HIGH;
    (VOID)tkl_gpio_read(ENC_E_PIN, &le);
    return (le == TUYA_GPIO_LEVEL_LOW) ? TRUE : FALSE;
}

/* 初始化编码器GPIO */
STATIC VOID __encoder_gpio_init(VOID)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_HIGH,
    };

    (VOID)tkl_gpio_init(ENC_A_PIN, &cfg);
    (VOID)tkl_gpio_init(ENC_B_PIN, &cfg);
    (VOID)tkl_gpio_init(ENC_E_PIN, &cfg);
}

/* 应用状态（带渐变） */
STATIC VOID __apply_state_with_fade(VOID)
{
    UINT16_T target_cold = 0;
    UINT16_T target_warm = 0;

    if (g_power_on) {
        /* 开灯：根据灯光模式计算占空比 */
        light_compute_target_duty(&target_cold, &target_warm);
    } else {
        /* 关灯：使用统一的电源控制函数（内置保存逻辑） */
        light_set_power(FALSE);
        target_cold = 0;
        target_warm = 0;
    }

    if (target_cold == s_current_duty_cold && target_warm == s_current_duty_warm) {
        fade_stop();
        return;
    }

    fade_start(target_cold, target_warm, FADE_DURATION_MS);
}

/* 从最暗开启灯光：使用MIN_DUTY（0.5%）作为总占空比，恢复保存的色温 */
STATIC VOID __encoder_turn_on_from_darkest(UINT16_T *target_cold, UINT16_T *target_warm)
{
    UINT16_T saved_color_temp = s_color_value;  /* 恢复保存的色温 */
    
    /* 直接使用MIN_DUTY（0.5%）作为总占空比 */
    UINT32_T total_duty_calc = MIN_DUTY;
    
    /* 根据保存的色温分配冷光和暖光占空比 */
    UINT32_T cold_ratio = saved_color_temp;
    UINT32_T warm_ratio = 1000 - saved_color_temp;
    UINT32_T cold_duty = total_duty_calc * cold_ratio / 1000;
    UINT32_T warm_duty = total_duty_calc * warm_ratio / 1000;
    *target_cold = (UINT16_T)cold_duty;
    *target_warm = (UINT16_T)warm_duty;
    
    /* 确保每个灯都不小于MIN_DUTY（如果总亮度足够） */
    if (total_duty_calc >= MIN_DUTY * 2) {
        if (*target_cold > 0 && *target_cold < MIN_DUTY) {
            *target_cold = MIN_DUTY;
            *target_warm = (UINT16_T)total_duty_calc - MIN_DUTY;
        } else if (*target_warm > 0 && *target_warm < MIN_DUTY) {
            *target_warm = MIN_DUTY;
            *target_cold = (UINT16_T)total_duty_calc - MIN_DUTY;
        }
    }
}

/* 编码器任务 */
STATIC VOID __encoder_task(VOID *arg)
{
    (VOID)arg;

    static const INT8_T qdec_lut[16] = {
        0, +1, -1, 0,
       -1,  0,  0, +1,
       +1,  0,  0, -1,
        0, -1, +1, 0,
    };

    UINT8_T last_ab = __encoder_read_ab();
    BOOL_T last_btn = __encoder_button_active_low();
    UINT32_T btn_last_change = tal_system_get_millisecond();
    BOOL_T btn_pressed = last_btn;
    UINT32_T btn_press_start = btn_pressed ? btn_last_change : 0;
    BOOL_T rotation_during_press = FALSE;

    for (;;) {
        UINT32_T now = tal_system_get_millisecond();
        BOOL_T btn_now = __encoder_button_active_low();

        /* 检测复位：长按编码器按钮5秒以上（和触摸板一样的复位逻辑） */
        if (btn_pressed && !rotation_during_press) {
            UINT32_T held_ms = now - btn_press_start;
            if (held_ms >= ENCODER_RESET_THRESHOLD_MS) {
                /* 配网指示灯期间忽略复位操作 */
                if (light_control_network_indicator_active()) {
                    TAL_PR_NOTICE("[HARDWARE] 编码器复位操作被忽略：配网灯光期间不允许硬件操作");
                    btn_press_start = now;  /* 重置计时，避免重复触发 */
                } else {
                    TAL_PR_NOTICE("Encoder reset detected (5s), removing device from cloud...");
                    extern OPERATE_RET tuya_iot_wf_gw_unactive(VOID);
                    OPERATE_RET ret = tuya_iot_wf_gw_unactive();
                    if (ret == OPRT_OK) {
                        TAL_PR_NOTICE("Device removed from cloud successfully, will enter provisioning mode");
                    } else {
                        TAL_PR_ERR("Failed to remove device from cloud, ret=%d", ret);
                    }
                    btn_press_start = now;  /* 重置计时，避免重复触发 */
                }
            }
        }

        if (btn_now != last_btn) {
            if (now - btn_last_change >= ENCODER_BUTTON_DEBOUNCE_MS) {
                last_btn = btn_now;
                btn_last_change = now;

                if (btn_now) {
                    btn_pressed = TRUE;
                    btn_press_start = now;
                    rotation_during_press = FALSE;
                } else {
                    if (btn_pressed && !rotation_during_press) {
                        /* 编码器按钮单击：开关灯（和触摸板一样的逻辑） */
                        /* 配网指示灯期间忽略操作 */
                        if (light_control_network_indicator_active()) {
                            TAL_PR_NOTICE("[HARDWARE] 编码器按钮操作被忽略：配网灯光期间不允许硬件操作");
                        } else {
                            /* 检查优先级：如果正在渐变或有更高优先级的控制，不接受开关命令 */
                            if (fade_is_active() || 
                                light_control_get_current() >= LIGHT_CONTROL_PRIO_TOUCH_CLICK) {
                                /* 优先级不够，忽略按钮操作 */
                            } else {
                                /* 请求触摸板单击控制权限（编码器按钮使用相同优先级） */
                                #define ENCODER_BUTTON_OWNER_ID  0x2002
                                LIGHT_CONTROL_TOKEN token = light_control_request(ENCODER_BUTTON_OWNER_ID, LIGHT_CONTROL_PRIO_TOUCH_CLICK);
                                if (token != LIGHT_CONTROL_TOKEN_INVALID) {
                                    /* 使用和触摸板一样的开关灯逻辑（light_control_set会自动处理保存和恢复） */
                                    BOOL_T current_state = s_light_power;
                                    light_control_set(!current_state);
                                    /* 上传状态到app（和触摸板一样） */
                                    upload_device_all_status();
                                    /* 释放控制权限 */
                                    light_control_release(token);
                                }
                            }
                        }
                    }
                    btn_pressed = FALSE;
                }
            }
        }

        /* 配网指示灯期间忽略所有编码器旋转操作 */
        if (light_control_network_indicator_active()) {
            /* 跳过旋转检测，但继续处理按钮 */
            last_ab = __encoder_read_ab();
        } else {
            UINT8_T ab_now = __encoder_read_ab();
            if (ab_now != last_ab) {
                UINT8_T idx = (UINT8_T)((last_ab << 2) | ab_now);
                INT8_T delta = qdec_lut[idx];

                if (delta != 0) {
                    /* 累积delta值并计算detent（格数）：4个delta = 1个detent */
                    s_encoder_accumulated_delta += delta;
                    INT32_T complete_detents = s_encoder_accumulated_delta / ENCODER_COUNTS_PER_DETENT;
                    INT32_T remainder = s_encoder_accumulated_delta % ENCODER_COUNTS_PER_DETENT;
                    
                    /* 处理负数余数：确保remainder在[0, ENCODER_COUNTS_PER_DETENT)范围内 */
                    if (remainder < 0) {
                        remainder += ENCODER_COUNTS_PER_DETENT;
                        complete_detents--;
                    }
                    
                    s_encoder_accumulated_delta = remainder;
                    
                    /* 请求编码器控制权限（最高优先级） */
                    #define ENCODER_OWNER_ID  0x2001
                    if (s_encoder_token == LIGHT_CONTROL_TOKEN_INVALID) {
                        s_encoder_token = light_control_request(ENCODER_OWNER_ID, LIGHT_CONTROL_PRIO_ENCODER);
                    }
                    if (s_encoder_token == LIGHT_CONTROL_TOKEN_INVALID) {
                        /* 优先级不够，忽略此次旋转 */
                        last_ab = ab_now;
                        continue;
                    }
                    
                    s_encoder_last_rotation_time = now;  /* 更新旋转时间 */
                    
                    INT8_T direction = (delta > 0) ? 1 : -1;
                    UINT16_T target_cold = 0;
                    UINT16_T target_warm = 0;

                    if (btn_pressed) {
                        /* 按下按钮时旋转：调整色温（冷灯和暖灯的比例） */
                        rotation_during_press = TRUE;
                        
                        UINT16_T total_duty = s_current_duty_cold + s_current_duty_warm;
                        
                        if (total_duty == 0) {
                            /* 如果当前关闭，从最暗开启：使用MIN_DUTY（0.5%）和保存的色温 */
                            __encoder_turn_on_from_darkest(&target_cold, &target_warm);
                            
                            /* 应用PWM并设置电源状态 */
                            light_apply_pwm_direct(target_cold, target_warm);
                            light_set_power(TRUE);
                            fade_stop();  /* 停止可能存在的渐变 */
                            last_ab = ab_now;
                            continue;
                        }
                        
                        /* 计算色温调整步进：在线模式1%，离线模式0.8% */
                        INT32_T temp_step;
                        if (get_run_mode() == RUN_MODE_OFFLINE) {
                            /* 离线模式：0.8% */
                            temp_step = (INT32_T)PWM_DUTY_MAX * (INT32_T)direction / 125;
                        } else {
                            /* 在线模式：1% */
                            temp_step = (INT32_T)PWM_DUTY_MAX * (INT32_T)direction / 100;
                        }
                        if (temp_step == 0) {
                            temp_step = direction * 5;  /* 最小步进 */
                        }
                        
                        INT32_T new_cold = (INT32_T)s_current_duty_cold - temp_step;  /* 右旋：冷灯变暗，暖灯变亮 */
                        INT32_T new_warm = (INT32_T)s_current_duty_warm + temp_step;
                        
                        /* 限制范围 */
                        if (new_cold < 0) {
                            new_cold = 0;
                            new_warm = total_duty;
                        } else if (new_cold > (INT32_T)total_duty) {
                            new_cold = total_duty;
                            new_warm = 0;
                        } else {
                            new_warm = total_duty - new_cold;
                        }
                        
                        /* 确保每个灯都不小于MIN_DUTY（如果总亮度足够） */
                        if (total_duty >= MIN_DUTY * 2) {
                            if (new_cold > 0 && new_cold < MIN_DUTY) {
                                new_cold = MIN_DUTY;
                                new_warm = total_duty - MIN_DUTY;
                            } else if (new_warm > 0 && new_warm < MIN_DUTY) {
                                new_warm = MIN_DUTY;
                                new_cold = total_duty - MIN_DUTY;
                            }
                        }
                        
                        target_cold = (UINT16_T)new_cold;
                        target_warm = (UINT16_T)new_warm;
                        /* 确保电源状态为开（通过light_set_power会保存状态） */
                        if (!g_power_on) {
                            light_set_power(TRUE);
                        }
                    } else {
                        /* 不按按钮时旋转：调整亮度（根据当前状态，不使用保存值） */
                        UINT16_T total_duty = s_current_duty_cold + s_current_duty_warm;
                        
                        if (total_duty == 0 && delta > 0) {
                            /* 如果当前关闭，从最暗开启：使用MIN_DUTY（0.5%）和保存的色温 */
                            __encoder_turn_on_from_darkest(&target_cold, &target_warm);
                            
                            /* 应用PWM并设置电源状态 */
                            light_apply_pwm_direct(target_cold, target_warm);
                            light_set_power(TRUE);
                            fade_stop();  /* 停止可能存在的渐变 */
                            last_ab = ab_now;
                            continue;
                        }
                        
                        if (total_duty > 0) {
                            /* 计算亮度调整步进：在线模式1%，离线模式0.8% */
                            INT32_T brightness_step;
                            if (get_run_mode() == RUN_MODE_OFFLINE) {
                                /* 离线模式：0.8% */
                                brightness_step = (INT32_T)PWM_DUTY_MAX * (INT32_T)direction / 125;
                            } else {
                                /* 在线模式：1% */
                                brightness_step = (INT32_T)PWM_DUTY_MAX * (INT32_T)direction / 100;
                            }
                            if (brightness_step == 0) {
                                brightness_step = direction * 5;  /* 最小步进 */
                            }
                            
                            INT32_T new_total = (INT32_T)total_duty + brightness_step;
                            
                            if (new_total < MIN_DUTY) {
                                new_total = MIN_DUTY;
                            } else if (new_total > PWM_DUTY_MAX) {
                                new_total = PWM_DUTY_MAX;
                            }
                            
                            /* 保持当前比例 */
                            UINT32_T cold_ratio = ((UINT32_T)s_current_duty_cold * 1000) / total_duty;
                            target_cold = (UINT16_T)(((UINT32_T)new_total * cold_ratio + 500) / 1000);
                            target_warm = (UINT16_T)new_total - target_cold;
                            
                            /* 确保每个灯都不小于MIN_DUTY（如果总亮度足够） */
                            if (new_total >= MIN_DUTY * 2) {
                                if (target_cold > 0 && target_cold < MIN_DUTY) {
                                    target_cold = MIN_DUTY;
                                    target_warm = (UINT16_T)new_total - MIN_DUTY;
                                } else if (target_warm > 0 && target_warm < MIN_DUTY) {
                                    target_warm = MIN_DUTY;
                                    target_cold = (UINT16_T)new_total - MIN_DUTY;
                                }
                            }
                            
                            /* 确保电源状态为开（通过light_set_power会保存状态） */
                            if (!g_power_on) {
                                light_set_power(TRUE);
                            }
                        }
                    }
                    
                    /* 直接应用PWM值，不使用渐变 */
                    if (target_cold != s_current_duty_cold || target_warm != s_current_duty_warm) {
                        fade_stop();  /* 停止可能存在的渐变 */
                        light_apply_pwm_direct(target_cold, target_warm);
                        
                        /* 配网指示灯期间不标记保存上传 */
                        if (!light_control_network_indicator_active()) {
                            /* 标记需要延迟保存和上传状态 */
                            s_encoder_last_state_change_time = now;
                            s_encoder_pending_save_upload = TRUE;
                        }
                    }
                }

                last_ab = ab_now;
            } else {
                /* 没有检测到旋转变化，检查是否超时后释放编码器控制权限 */
                if (s_encoder_token != LIGHT_CONTROL_TOKEN_INVALID) {
                    /* 如果距离上次旋转超过200ms，释放权限（给渐变一些时间继续） */
                    if (s_encoder_last_rotation_time > 0 && 
                        (now - s_encoder_last_rotation_time) > 200) {
                        light_control_release(s_encoder_token);
                        s_encoder_token = LIGHT_CONTROL_TOKEN_INVALID;
                        s_encoder_last_rotation_time = 0;
                    }
                }
                
                /* 检查是否需要延迟保存和上传状态 */
                if (s_encoder_pending_save_upload && s_encoder_last_state_change_time > 0) {
                    UINT32_T elapsed = (now >= s_encoder_last_state_change_time) ?
                                      (now - s_encoder_last_state_change_time) :
                                      (0xFFFFFFFF - s_encoder_last_state_change_time + now);
                    
                    if (elapsed >= ENCODER_SAVE_UPLOAD_DELAY_MS) {
                        /* 配网指示灯期间忽略保存和上传 */
                        if (light_control_network_indicator_active()) {
                            /* 取消保存上传，清除标记 */
                            s_encoder_pending_save_upload = FALSE;
                            s_encoder_last_state_change_time = 0;
                        } else {
                            /* 旋转停止后延迟时间已到，执行保存和上传 */
                            /* 计算当前亮度和色温值 */
                            UINT16_T total_duty = s_current_duty_cold + s_current_duty_warm;
                            UINT16_T brightness = 0;
                            UINT16_T color_temp = 500;  /* 默认中间色温 */
                            
                            if (total_duty > 0) {
                                /* 计算亮度：总占空比 / PWM_DUTY_MAX * 1000 */
                                brightness = (UINT16_T)((total_duty * 1000 + PWM_DUTY_MAX / 2) / PWM_DUTY_MAX);
                                if (brightness > 1000) {
                                    brightness = 1000;
                                }
                                
                                /* 计算色温：冷光比例 * 1000 */
                                UINT32_T cold_ratio = ((UINT32_T)s_current_duty_cold * 1000) / total_duty;
                                color_temp = (UINT16_T)cold_ratio;
                            }
                            
                            /* 同步更新系统内部状态，确保上传时使用正确的值 */
                            s_brightness_value = brightness;
                            s_color_value = color_temp;
                            
                            /* 保存状态到KV（内部有节流机制，不会频繁写入） */
                            dp_update_realtime_light_state(s_light_power, brightness, color_temp);
                            
                            /* 上传状态到app */
                            upload_device_all_status();
                            
                            s_encoder_pending_save_upload = FALSE;
                            s_encoder_last_state_change_time = 0;
                        }
                    }
                }
            }
        }

        tal_system_sleep(ENCODER_POLL_INTERVAL_MS);
    }
}

/* 初始化编码器模块 */
VOID encoder_init(VOID)
{
    __encoder_gpio_init();
    
    THREAD_CFG_T encoder_cfg = {
        .thrdname   = "encoder_ctrl",
        .priority   = THREAD_PRIO_6,
        .stackDepth = 2048,
    };
    (VOID)tal_thread_create_and_start(&s_encoder_thread, NULL, NULL, __encoder_task, NULL, &encoder_cfg);
}

/* 编码器任务启动（兼容性函数） */
VOID encoder_start_task(VOID)
{
    /* 任务已在encoder_init中启动，这里为空实现以保持兼容性 */
}

