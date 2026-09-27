/**
 * @file tuya_app_main.c
 * @author www.tuya.com
 * @brief tuya_app_main module is used to 
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include <stdlib.h>
#include <math.h>

#include "tuya_cloud_types.h"
#include "tuya_svc_netmgr.h"
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
#include "tuya_iot_wifi_api.h"
#include "tkl_wifi.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "tuya_iot_base_api.h"
#endif
#include "tuya_iot_com_api.h"
#include "tuya_ws_db.h"

#include "tal_system.h"
#include "tal_log.h"
#include "base_event.h"
#include "mqc_app.h"
#if defined(ENABLE_LWIP) && (ENABLE_LWIP == 1)
#include "lwip_init.h"
#endif

#include "tal_thread.h"
#include "tal_time_service.h"
#include "tkl_pwm.h"
#include "dp_process.h"
#include "tal_bluetooth.h"
#include "offline_mode.h"

/* 外部函数声明 */
extern VOID dp_update_realtime_light_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);

/* 外部变量声明：离线模式渐变状态 */
extern volatile BOOL_T s_offline_fade_active;
extern volatile UINT16_T s_offline_current_duty_cold;
extern volatile UINT16_T s_offline_current_duty_warm;

/* 外部变量声明：硬件控制优先级 */
extern volatile BOOL_T s_hardware_control_active;
extern volatile UINT32_T s_hardware_control_timeout_ms;
#define HARDWARE_CONTROL_TIMEOUT_MS 500  /* 硬件操作后500ms内，APP控制被忽略 */

#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
extern OPERATE_RET tuya_wifi_netcfg_stop(VOID);
#endif

/***********************************************************
************************macro define************************
***********************************************************/

#define PID         "YOUR_PID"
/* 警告：不要硬编码UUID和AUTHKEY，否则OTA升级会导致所有设备使用相同的授权信息，造成冲突！
 * 正确的做法：
 * 1. 生产时使用涂鸦产测工具为每个设备烧录唯一的授权信息到Flash
 * 2. 代码中注释掉UUID和AUTHKEY定义，让设备从Flash读取授权信息
 * 3. 这样OTA升级时不会覆盖设备原有的授权信息
 */
// #define UUID        "YOUR_UUID"
// #define AUTHKEY     "YOUR_AUTHKEY"

#define PWM_COLD_CHANNEL               PWM_NUM_1
#define PWM_WARM_CHANNEL               PWM_NUM_2
#define PWM_FREQUENCY_HZ               10000
#define PWM_DUTY_MAX                   1500

#define FADE_DURATION_MS               3000
#define FADE_UPDATE_INTERVAL_MS        20
#define FADE_SIGMOID_STEEPNESS         10.0f
#define FADE_ADAPTIVE_MIN_MS           50
#define FADE_ADAPTIVE_MAX_MS           200
#define BRIGHT_VALUE_MIN               10
#define BRIGHT_VALUE_MAX               1000
#define FIRST_BOOT_BREATH_MS           30000   /* 30s 闪烁 */
#define FIRST_BOOT_TOTAL_MS            180000  /* 配网窗口 3min（含前30s闪烁） */
#define FIRST_BOOT_CONSTANT_MS         (FIRST_BOOT_TOTAL_MS - FIRST_BOOT_BREATH_MS) /* 闪烁后常亮时长 */
#define POST_BOOT_GUARD_MS             60000
#define RESET_WINDOW_SECONDS           120
#define RESET_CYCLES_REQUIRED          4
#define KV_KEY_FIRST_PROVISION_DONE    "first_prov_done"  /* 首次配网完成标记 */
/* The registration code here does not work, you need to apply for a new one.
 * https://developer.tuya.com/cn/docs/iot/lisence-management?id=Kb4qlem97idl0
 */
// #define UUID     "YOUR_UUID"
// #define AUTHKEY  "YOUR_AUTHKEY"

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/
extern void tuya_ble_enable_debug(bool enable);
STATIC VOID __start_network_indicator_sequence(BOOL_T first_boot);
STATIC VOID __first_boot_flow(VOID);
STATIC BOOL_T __do_not_disturb_should_block_light(VOID);
STATIC VOID __start_cloud_guard_task(VOID);
STATIC VOID __cloud_guard_task(VOID *arg);
STATIC BOOL_T __is_first_provision_done(VOID);
STATIC VOID __mark_first_provision_done(VOID);
STATIC VOID __clear_first_provision_done(VOID);

/***********************************************************
***********************variable define**********************
***********************************************************/
/* app thread handle */
STATIC THREAD_HANDLE ty_app_thread = NULL;
BOOL_T s_light_power = FALSE;
STATIC THREAD_HANDLE s_fade_thread = NULL;
UINT16_T s_brightness_value = 1000;
STATIC UINT16_T s_last_nonzero_brightness = 1000;
UINT16_T s_color_value = 500;
volatile BOOL_T   s_fade_active = FALSE;
STATIC volatile UINT32_T s_fade_start_ms = 0;
STATIC volatile UINT16_T s_fade_start_duty_cold = 0;
STATIC volatile UINT16_T s_fade_start_duty_warm = 0;
volatile UINT16_T s_fade_target_duty_cold = 0;  /* 编码器模块需要访问此变量 */
volatile UINT16_T s_fade_target_duty_warm = 0;  /* 编码器模块需要访问此变量 */
volatile UINT16_T s_current_duty_cold = 0;
volatile UINT16_T s_current_duty_warm = 0;
STATIC volatile UINT32_T s_fade_duration_ms = FADE_DURATION_MS;
STATIC volatile BOOL_T s_fade_is_encoder_triggered = FALSE;  /* 标记是否是编码器触发的渐变 */
/* 标记当前是否为"编码器调光/调色温"操作，其他控制不受影响 */
volatile BOOL_T   s_encoder_dim_change = FALSE;
volatile UINT32_T s_encoder_dim_duration_ms = 1000;  /* 编码器调光渐变时长，默认1秒 */
STATIC volatile BOOL_T s_fade_use_linear = FALSE;  /* 标记当前渐变是否使用线性（编码器调光） */
STATIC UINT32_T s_gradual_on_duration_ms = FADE_DURATION_MS;
STATIC UINT32_T s_gradual_off_duration_ms = FADE_DURATION_MS;
STATIC BOOL_T s_gradual_on_enabled = TRUE;
STATIC BOOL_T s_gradual_off_enabled = TRUE;
STATIC THREAD_HANDLE s_network_indicator_thread = NULL;
STATIC volatile BOOL_T s_network_indicator_active = FALSE;
STATIC THREAD_HANDLE s_cloud_guard_thread = NULL;
STATIC volatile BOOL_T s_cloud_guard_stop = TRUE;
STATIC THREAD_HANDLE s_restore_save_enable_thread = NULL;
/* 组网成功标志：组网成功后，即使DP33恢复，也不关闭灯光 */
STATIC volatile BOOL_T s_provisioning_success_light_on = FALSE;
/* ============================================================
 * 运行模式状态位（全局变量，供offline_mode.c访问）
 * ============================================================
 * 修改此初始值来设置默认运行模式：
 *   RUN_MODE_OFFLINE = 离线模式（编码器/触摸板控制）
 *   RUN_MODE_ONLINE  = 在线模式（APP/云端控制）
 * 
 * 运行时切换模式请使用：set_run_mode(RUN_MODE_XXX)
 * ============================================================ */
volatile RUN_MODE_E s_current_run_mode = RUN_MODE_ONLINE;  /* 默认：在线模式 */
STATIC BOOL_T s_indicator_prev_power = FALSE;
STATIC UINT16_T s_indicator_prev_brightness = 1000;
STATIC UINT16_T s_indicator_prev_color = 500;

/* 解绑检测相关变量 */
#define UNBIND_DETECT_TIMEOUT_MS 30000  /* 30秒超时 */
STATIC volatile BOOL_T s_unbind_detect_active = FALSE;
STATIC volatile UINT32_T s_unbind_detect_start_ms = 0;
STATIC BOOL_T s_has_connected_mqtt_before = FALSE;  /* 之前是否连接过MQTT（用于区分首次配网和解绑） */
STATIC THREAD_HANDLE s_unbind_detect_thread = NULL;  /* 解绑检测线程句柄 */

/***********************************************************
***********************function define**********************
***********************************************************/

/* boot_state KV已移除，统一走首次启动流程 */

/* -------------------- 首次配网完成标记函数 -------------------- */
/**
 * @brief 检查是否已完成首次配网
 * @return TRUE=已完成首次配网，FALSE=未完成（解绑后的首次配网）
 */
STATIC BOOL_T __is_first_provision_done(VOID)
{
    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET ret = wd_common_read(KV_KEY_FIRST_PROVISION_DONE, &buf, &len);
    
    if (ret == OPRT_OK && buf != NULL && len > 0) {
        BOOL_T done = (buf[0] != 0);
        wd_common_free_data(buf);
        return done;
    }
    
    return FALSE;  /* 没有标记 = 未完成首次配网（解绑后的首次配网） */
}

/**
 * @brief 标记首次配网完成
 */
STATIC VOID __mark_first_provision_done(VOID)
{
    UINT8_T flag = 1;
    OPERATE_RET ret = wd_common_write(KV_KEY_FIRST_PROVISION_DONE, &flag, sizeof(flag));
    if (ret == OPRT_OK) {
        TAL_PR_NOTICE("[PROVISION] First provisioning marked as done");
    } else {
        TAL_PR_ERR("[PROVISION] Failed to mark first provisioning done, ret=%d", ret);
    }
}

/**
 * @brief 清除首次配网完成标记（解绑时调用）
 */
STATIC VOID __clear_first_provision_done(VOID)
{
    UINT8_T flag = 0;
    OPERATE_RET ret = wd_common_write(KV_KEY_FIRST_PROVISION_DONE, &flag, sizeof(flag));
    if (ret == OPRT_OK) {
        TAL_PR_NOTICE("[PROVISION] First provisioning flag cleared (unbind detected)");
    } else {
        TAL_PR_ERR("[PROVISION] Failed to clear first provisioning flag, ret=%d", ret);
    }
}

STATIC float __smooth_sigmoid_ease(float progress)
{
    if (progress <= 0.0f) {
        return 0.0f;
    }
    if (progress >= 1.0f) {
        return 1.0f;
    }

    float steep = FADE_SIGMOID_STEEPNESS;
    if (steep < 1.0f) {
        steep = 1.0f;
    }
    const float center = 0.55f;

    float logistic = 1.0f / (1.0f + expf(-steep * (progress - center)));
    float logistic_min = 1.0f / (1.0f + expf(-steep * (0.0f - center)));
    float logistic_max = 1.0f / (1.0f + expf(-steep * (1.0f - center)));

    float normalized = (logistic - logistic_min) / (logistic_max - logistic_min);
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    float smooth = normalized * normalized * normalized *
                   (normalized * (normalized * 6.0f - 15.0f) + 10.0f);

    if (smooth < 0.0f) smooth = 0.0f;
    if (smooth > 1.0f) smooth = 1.0f;

    /* 压暗前段：gamma 校正，让开始阶段更暗、后半段加速 */
    const float gamma = 2.2f;
    float adjusted = powf(smooth, gamma);
    if (adjusted < 0.0f) adjusted = 0.0f;
    if (adjusted > 1.0f) adjusted = 1.0f;

    return adjusted;
}

STATIC VOID __compute_target_duties(UINT16_T *cold_duty, UINT16_T *warm_duty)
{
    if (!s_light_power || s_brightness_value == 0) {
        *cold_duty = 0;
        *warm_duty = 0;
        return;
    }

    UINT32_T brightness = s_brightness_value;
    if (brightness > 1000) {
        brightness = 1000;
    }
    UINT32_T color = s_color_value;
    if (color > 1000) {
        color = 1000;
    }

    UINT32_T cold_ratio = color;
    UINT32_T warm_ratio = 1000 - color;

    UINT32_T cold = brightness * cold_ratio * PWM_DUTY_MAX;
    UINT32_T warm = brightness * warm_ratio * PWM_DUTY_MAX;

    *cold_duty = (UINT16_T)((cold + 500000) / 1000000);
    *warm_duty = (UINT16_T)((warm + 500000) / 1000000);

    if (*cold_duty > PWM_DUTY_MAX) {
        *cold_duty = PWM_DUTY_MAX;
    }
    if (*warm_duty > PWM_DUTY_MAX) {
        *warm_duty = PWM_DUTY_MAX;
    }
}

STATIC VOID __stop_fade(VOID)
{
    s_fade_active = FALSE;
    s_fade_is_encoder_triggered = FALSE;
}

STATIC VOID __apply_pwm_direct(UINT16_T duty_cold, UINT16_T duty_warm)
{
    OPERATE_RET rt = OPRT_OK;
    TUYA_CALL_ERR_LOG(tkl_pwm_duty_set(PWM_COLD_CHANNEL, duty_cold));
    TUYA_CALL_ERR_LOG(tkl_pwm_duty_set(PWM_WARM_CHANNEL, duty_warm));
    TUYA_CALL_ERR_LOG(tkl_pwm_start(PWM_COLD_CHANNEL));
    TUYA_CALL_ERR_LOG(tkl_pwm_start(PWM_WARM_CHANNEL));
    s_current_duty_cold = duty_cold;
    s_current_duty_warm = duty_warm;
    /* 同步更新离线模式的状态变量，确保状态一致 */
    s_offline_current_duty_cold = duty_cold;
    s_offline_current_duty_warm = duty_warm;
}

STATIC VOID __start_fade(UINT16_T target_duty_cold, UINT16_T target_duty_warm, UINT32_T duration_ms)
{
    if (duration_ms == 0) {
        __apply_pwm_direct(target_duty_cold, target_duty_warm);
        s_fade_active = FALSE;
        s_fade_is_encoder_triggered = FALSE;
        return;
    }

    /* 停止离线模式的渐变，避免冲突 */
    s_offline_fade_active = FALSE;

    s_fade_start_duty_cold = s_current_duty_cold;
    s_fade_start_duty_warm = s_current_duty_warm;
    s_fade_target_duty_cold = target_duty_cold;
    s_fade_target_duty_warm = target_duty_warm;
    s_fade_start_ms = tal_system_get_millisecond();
    s_fade_duration_ms = (duration_ms == 0) ? FADE_DURATION_MS : duration_ms;
    s_fade_active = TRUE;
    s_fade_is_encoder_triggered = FALSE;  /* 默认不是编码器触发 */
}

STATIC VOID __apply_state_immediate(VOID)
{
    __stop_fade();
    UINT16_T duty_cold = 0;
    UINT16_T duty_warm = 0;
    __compute_target_duties(&duty_cold, &duty_warm);
    __apply_pwm_direct(duty_cold, duty_warm);
}

STATIC VOID __set_light_immediate(UINT16_T brightness, UINT16_T color)
{
    if (brightness > 1000) {
        brightness = 1000;
    }
    if (color > 1000) {
        color = 1000;
    }

    if (brightness > 0) {
        s_light_power = TRUE;
        s_brightness_value = brightness;
        s_last_nonzero_brightness = brightness;
    } else {
        s_light_power = FALSE;
        s_brightness_value = 0;
    }

    s_color_value = color;
    __apply_state_immediate();
}

/* 供编码器旋转标记使用：在调用 light_control_set_* 前设置，在 __apply_state_with_fade 用后即清除 */
VOID light_control_set_encoder_dim_flag(BOOL_T enable)
{
    s_encoder_dim_change = enable;
    if (enable) {
        s_encoder_dim_duration_ms = 1000;  /* 默认1秒渐变（慢速旋转） */
    }
}

/* 供编码器快速旋转使用：设置自定义渐变时长 */
VOID light_control_set_encoder_dim_duration(UINT32_T duration_ms)
{
    s_encoder_dim_change = TRUE;
    s_encoder_dim_duration_ms = duration_ms;
}

STATIC VOID __apply_state_with_fade(UINT32_T duration_ms)
{
    UINT16_T duty_cold = 0;
    UINT16_T duty_warm = 0;
    __compute_target_duties(&duty_cold, &duty_warm);

    /* 编码器调光/调色温：使用设置的渐变时长（快速旋转300ms，慢速旋转1000ms）；其他沿用传入时长 */
    UINT32_T effective_duration = duration_ms;
    BOOL_T is_encoder_dim = s_encoder_dim_change;
    if (is_encoder_dim) {
        effective_duration = s_encoder_dim_duration_ms;
    }

    if ((duty_cold == s_current_duty_cold) && (duty_warm == s_current_duty_warm)) {
        __stop_fade();
        s_encoder_dim_change = FALSE;
        s_fade_use_linear = FALSE;
        return;
    }

    /* 保存是否使用线性渐变的标志 */
    s_fade_use_linear = is_encoder_dim;
    __start_fade(duty_cold, duty_warm, effective_duration);
    s_encoder_dim_change = FALSE;
}

STATIC VOID __fade_task(VOID *arg)
{
    (VOID)arg;

    while (1) {
        /* 如果离线模式的渐变正在运行，则停止在线模式的渐变并同步状态 */
        if (s_offline_fade_active) {
            if (s_fade_active) {
                TAL_PR_DEBUG("[FADE_TASK] Offline fade active, stopping online fade and syncing state.");
                s_fade_active = FALSE;
            }
            /* 同步在线模式的状态变量到离线模式的当前值 */
            s_current_duty_cold = s_offline_current_duty_cold;
            s_current_duty_warm = s_offline_current_duty_warm;
            tal_system_sleep(FADE_UPDATE_INTERVAL_MS);
            continue; /* 跳过在线模式的PWM更新，让离线模式控制 */
        }

        if (s_fade_active) {
            UINT32_T now = tal_system_get_millisecond();
            UINT32_T elapsed = now - s_fade_start_ms;

            if (elapsed >= s_fade_duration_ms) {
                /* 渐变完成，应用目标值 */
                if (s_fade_target_duty_cold != s_current_duty_cold || 
                    s_fade_target_duty_warm != s_current_duty_warm) {
                    __apply_pwm_direct(s_fade_target_duty_cold, s_fade_target_duty_warm);
                }
                s_fade_active = FALSE;
                s_fade_is_encoder_triggered = FALSE;  /* 渐变完成，清除编码器触发标志 */
            } else {
                float progress = (float)elapsed / (float)s_fade_duration_ms;
                /* 编码器调光使用线性渐变，其他使用sigmoid缓动 */
                float eased;
                if (s_fade_use_linear) {
                    /* 线性渐变 */
                    eased = progress;
                } else {
                    /* Sigmoid缓动 */
                    eased = __smooth_sigmoid_ease(progress);
                }
                float duty_cold = (float)s_fade_start_duty_cold +
                                  ((float)s_fade_target_duty_cold - (float)s_fade_start_duty_cold) * eased;
                float duty_warm = (float)s_fade_start_duty_warm +
                                  ((float)s_fade_target_duty_warm - (float)s_fade_start_duty_warm) * eased;
                UINT16_T new_cold = (UINT16_T)(duty_cold + 0.5f);
                UINT16_T new_warm = (UINT16_T)(duty_warm + 0.5f);
                /* 只有当计算出的值与当前值不同时才更新PWM，避免重复调用 */
                if (new_cold != s_current_duty_cold || new_warm != s_current_duty_warm) {
                    __apply_pwm_direct(new_cold, new_warm);
                }
            }
        }

        tal_system_sleep(FADE_UPDATE_INTERVAL_MS);
    }
}

STATIC VOID __light_pwm_init(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    TUYA_PWM_BASE_CFG_T cfg = {
        .polarity  = TUYA_PWM_POSITIVE,
        .duty      = 0,
        .frequency = PWM_FREQUENCY_HZ,
    };

    TUYA_CALL_ERR_LOG(tkl_pwm_init(PWM_COLD_CHANNEL, &cfg));
    TUYA_CALL_ERR_LOG(tkl_pwm_start(PWM_COLD_CHANNEL));
    TUYA_CALL_ERR_LOG(tkl_pwm_init(PWM_WARM_CHANNEL, &cfg));
    TUYA_CALL_ERR_LOG(tkl_pwm_start(PWM_WARM_CHANNEL));

    s_current_duty_cold = 0;
    s_current_duty_warm = 0;
    if (s_brightness_value == 0) {
        s_brightness_value = 1000;
    }
    s_last_nonzero_brightness = s_brightness_value;
    if (s_color_value > 1000) {
        s_color_value = 500;
    }

    THREAD_CFG_T fade_cfg = {
        .thrdname   = "fade_ctrl",
        .priority   = THREAD_PRIO_6,
        .stackDepth = 1024,
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(&s_fade_thread, NULL, NULL, __fade_task, NULL, &fade_cfg));
}

STATIC VOID __light_apply_state(VOID)
{
    __apply_state_immediate();
}

VOID light_control_set(BOOL_T on)
{
    /* 注意：模式切换只能通过 set_run_mode() 手动设置，不会自动切换 */
    /* 只有在 RUN_MODE_ONLINE 模式下才会执行PWM控制 */
    
    /* 取消APP侧的硬件屏蔽，改为后到的命令覆盖前到的命令 */
    
    BOOL_T new_state = on ? TRUE : FALSE;

    if (new_state) {
        s_light_power = TRUE;
        if (s_brightness_value == 0) {
            s_brightness_value = s_last_nonzero_brightness ? s_last_nonzero_brightness : 1000;
        }
        /* 如果色温为0，保持为0（不设置默认值） */
    } else {
        if (s_brightness_value > 0) {
            s_last_nonzero_brightness = s_brightness_value;
        }
        s_light_power = FALSE;
        s_brightness_value = 0;  /* 关灯时亮度值设为0 */
    }

    UINT32_T duration = 0;
    if (s_light_power) {
        duration = s_gradual_on_enabled ? s_gradual_on_duration_ms : 0;
    } else {
        duration = s_gradual_off_enabled ? s_gradual_off_duration_ms : 0;
    }

    __apply_state_with_fade(duration);
    
    /* 更新实时灯光状态到持久化存储 */
    /* 如果灯是开着的但色温为0，保存时使用0 */
    dp_update_realtime_light_state(s_light_power, s_brightness_value, s_color_value);
}

BOOL_T light_control_get(VOID)
{
    return s_light_power;
}

VOID light_control_set_brightness(UINT16_T value)
{
    /* 注意：模式切换只能通过 set_run_mode() 手动设置，不会自动切换 */
    /* 只有在 RUN_MODE_ONLINE 模式下才会执行PWM控制 */
    
    /* 取消APP侧的硬件屏蔽，改为后到的命令覆盖前到的命令 */
    
    if (value > 1000) {
        value = 1000;
    }
    s_brightness_value = value;
    if (s_brightness_value > 0) {
        s_last_nonzero_brightness = s_brightness_value;
    }

    if (s_light_power) {
        UINT32_T duration = s_gradual_on_enabled ? s_gradual_on_duration_ms : 0;
        __apply_state_with_fade(duration);
    }
    
    /* 更新实时灯光状态到持久化存储 */
    /* 如果灯是开着的但色温为0，保存时使用0 */
    dp_update_realtime_light_state(s_light_power, s_brightness_value, s_color_value);
}

UINT16_T light_control_get_brightness(VOID)
{
    return s_brightness_value;
}

VOID light_control_set_color_temp(UINT16_T value)
{
    /* 注意：模式切换只能通过 set_run_mode() 手动设置，不会自动切换 */
    /* 只有在 RUN_MODE_ONLINE 模式下才会执行PWM控制 */
    
    /* 取消APP侧的硬件屏蔽，改为后到的命令覆盖前到的命令 */
    
    if (value > 1000) {
        value = 1000;
    }
    s_color_value = value;

    if (s_light_power) {
        UINT32_T duration = s_gradual_on_enabled ? s_gradual_on_duration_ms : 0;
        __apply_state_with_fade(duration);
    }
    
    /* 更新实时灯光状态到持久化存储 */
    /* 如果灯是开着的但色温为0，保存时使用0 */
    dp_update_realtime_light_state(s_light_power, s_brightness_value, s_color_value);
}

UINT16_T light_control_get_color_temp(VOID)
{
    return s_color_value;
}

/**
 * @brief 批量设置灯光状态（开关、亮度、色温），只启动一次渐变
 * @param switch_on 开关状态
 * @param brightness 亮度值 (0-1000)
 * @param color_temp 色温值 (0-1000)
 * @note 此函数用于断电恢复等场景，避免连续多次启动渐变导致状态混乱
 */
VOID light_control_set_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp)
{
    /* 取消APP侧的硬件屏蔽，改为后到的命令覆盖前到的命令 */

    /* 如果组网成功标志已设置，且尝试关闭灯光，则强制保持常亮 */
    if (s_provisioning_success_light_on && !switch_on) {
        TAL_PR_NOTICE("[LIGHT] Provisioning success flag is set, forcing light ON (100%%, 50%% color temp) instead of OFF");
        switch_on = TRUE;
        brightness = 1000;
        color_temp = 500;
    }

    /* 参数校验 */
    if (brightness > 1000) {
        brightness = 1000;
    }
    if (color_temp > 1000) {
        color_temp = 1000;
    }
    if (switch_on && brightness < BRIGHT_VALUE_MIN) {
        brightness = BRIGHT_VALUE_MIN;
    }

    /* 先停止当前渐变，避免状态混乱 */
    __stop_fade();

    /* 更新状态变量 */
    BOOL_T new_state = switch_on ? TRUE : FALSE;
    
    if (new_state) {
        s_light_power = TRUE;
        if (brightness == 0) {
            brightness = s_last_nonzero_brightness ? s_last_nonzero_brightness : 1000;
        }
    } else {
        if (s_brightness_value > 0) {
            s_last_nonzero_brightness = s_brightness_value;
        }
        s_light_power = FALSE;
        brightness = 0;  /* 关闭时亮度为0 */
    }

    s_brightness_value = brightness;
    s_color_value = color_temp;

    /* 只启动一次渐变 */
    UINT32_T duration = 0;
    if (s_light_power) {
        duration = s_gradual_on_enabled ? s_gradual_on_duration_ms : 0;
    } else {
        duration = s_gradual_off_enabled ? s_gradual_off_duration_ms : 0;
    }

    __apply_state_with_fade(duration);
    
    /* 更新实时灯光状态到持久化存储 */
    dp_update_realtime_light_state(s_light_power, s_brightness_value, s_color_value);
}

VOID light_control_update_switch_gradient(UINT32_T on_time_ms, UINT32_T off_time_ms)
{
    const UINT32_T MAX_GRADUAL_MS = 10000; /* 10s 上限，匹配APP最大设置值 */

    if (on_time_ms > MAX_GRADUAL_MS) {
        on_time_ms = MAX_GRADUAL_MS;
        TAL_PR_WARN("Fade in time exceeds max (10s), clamped to 10000 ms");
    }
    if (off_time_ms > MAX_GRADUAL_MS) {
        off_time_ms = MAX_GRADUAL_MS;
        TAL_PR_WARN("Fade out time exceeds max (10s), clamped to 10000 ms");
    }

    s_gradual_on_duration_ms = on_time_ms;
    s_gradual_off_duration_ms = off_time_ms;
    s_gradual_on_enabled = (on_time_ms > 0);
    s_gradual_off_enabled = (off_time_ms > 0);

    TAL_PR_NOTICE("Switch gradient updated: on=%lu ms (enable=%d), off=%lu ms (enable=%d)",
                  (unsigned long)on_time_ms,
                  s_gradual_on_enabled,
                  (unsigned long)off_time_ms,
                  s_gradual_off_enabled);
}

BOOL_T light_control_network_indicator_active(VOID)
{
    return s_network_indicator_active ? TRUE : FALSE;
}

VOID light_control_get_switch_gradient(UINT32_T *on_time_ms, UINT32_T *off_time_ms)
{
    if (on_time_ms) {
        *on_time_ms = s_gradual_on_duration_ms;
    }
    if (off_time_ms) {
        *off_time_ms = s_gradual_off_duration_ms;
    }
}

/* 适配1103编码器逻辑的公开函数 */
VOID light_apply_pwm_direct(UINT16_T duty_cold, UINT16_T duty_warm)
{
    __apply_pwm_direct(duty_cold, duty_warm);
}

VOID fade_start(UINT16_T target_cold, UINT16_T target_warm, UINT32_T duration_ms)
{
    __start_fade(target_cold, target_warm, duration_ms);
}

VOID fade_update_target(UINT16_T target_cold, UINT16_T target_warm)
{
    /* 更新渐变目标值（不重启渐变，用于连续旋转时平滑过渡） */
    if (s_fade_active && s_fade_is_encoder_triggered) {
        /* 如果正在渐变且是编码器触发的，更新目标值和起始值 */
        /* 将当前值作为新的起始值，实现平滑过渡 */
        s_fade_start_duty_cold = s_current_duty_cold;
        s_fade_start_duty_warm = s_current_duty_warm;
        s_fade_target_duty_cold = target_cold;
        s_fade_target_duty_warm = target_warm;
        s_fade_start_ms = tal_system_get_millisecond();  /* 重置时间，重新开始渐变 */
    } else {
        /* 如果不在渐变或不是编码器触发，正常启动渐变 */
        __start_fade(target_cold, target_warm, FADE_DURATION_MS);
        s_fade_is_encoder_triggered = TRUE;  /* 标记为编码器触发 */
    }
}

VOID fade_stop(VOID)
{
    __stop_fade();
    s_fade_is_encoder_triggered = FALSE;
}

BOOL_T fade_is_active(VOID)
{
    return s_fade_active;
}

VOID light_compute_target_duty(UINT16_T *duty_cold, UINT16_T *duty_warm)
{
    __compute_target_duties(duty_cold, duty_warm);
}

OPERATE_RET light_set_power(BOOL_T on)
{
    if (on) {
        if (!s_light_power) {
            s_light_power = TRUE;
            if (s_brightness_value == 0) {
                s_brightness_value = s_last_nonzero_brightness > 0 ? s_last_nonzero_brightness : 100;
            }
            __apply_state_with_fade(s_gradual_on_enabled ? s_gradual_on_duration_ms : 0);
        }
    } else {
        if (s_light_power) {
            s_last_nonzero_brightness = s_brightness_value;
            s_light_power = FALSE;
            s_brightness_value = 0;
            __apply_state_with_fade(s_gradual_off_enabled ? s_gradual_off_duration_ms : 0);
        }
    }
    return OPRT_OK;
}

/* ========== 优先级控制系统（适配1103逻辑） ========== */
/* 控制优先级定义（数值越大优先级越高） */
typedef enum {
    LIGHT_CONTROL_PRIO_NONE = 0,      /* 无控制 */
    LIGHT_CONTROL_PRIO_FADE = 1,      /* 普通渐变 */
    LIGHT_CONTROL_PRIO_TOUCH_CLICK = 2,  /* 触摸板单击/双击 */
    LIGHT_CONTROL_PRIO_TOUCH_LONG = 3,   /* 触摸板长按 */
    LIGHT_CONTROL_PRIO_ENCODER = 4,   /* 编码器旋转（最高优先级） */
} LIGHT_CONTROL_PRIO_E;

/* 优先级控制函数（带owner ID） */
typedef UINT32_T LIGHT_CONTROL_TOKEN;  /* 控制令牌，用于释放权限 */
#define LIGHT_CONTROL_TOKEN_INVALID  ((LIGHT_CONTROL_TOKEN)0)  /* 无效令牌 */

/* Token生成辅助宏（限制owner_id到16位） */
#define LIGHT_CONTROL_OWNER_MASK     0xFFFFu
#define LIGHT_CONTROL_COUNTER_MASK   0xFFFFu

/* 控制优先级管理（带owner ID和超时机制） */
STATIC volatile LIGHT_CONTROL_PRIO_E s_current_control_prio = LIGHT_CONTROL_PRIO_NONE;
STATIC volatile UINT32_T s_current_control_owner = 0;  /* 当前控制者ID */
STATIC volatile UINT32_T s_token_counter = 1;  /* 令牌计数器 */
STATIC volatile UINT32_T s_control_start_time = 0;  /* 控制开始时间（用于超时检测） */
STATIC volatile LIGHT_CONTROL_TOKEN s_current_token = LIGHT_CONTROL_TOKEN_INVALID;  /* 当前有效token */

/* 控制超时时间（30秒，防止控制被永久锁定） */
#define LIGHT_CONTROL_TIMEOUT_MS  30000

/* Token生成辅助函数 */
STATIC inline LIGHT_CONTROL_TOKEN make_token(UINT32_T owner_id, UINT32_T counter)
{
    UINT32_T o = owner_id & LIGHT_CONTROL_OWNER_MASK;
    UINT32_T c = counter & LIGHT_CONTROL_COUNTER_MASK;
    return (LIGHT_CONTROL_TOKEN)((o << 16) | c);
}

/* 请求控制权限（最高优先级编码器） */
LIGHT_CONTROL_TOKEN light_control_request(UINT32_T owner_id, LIGHT_CONTROL_PRIO_E priority)
{
    LIGHT_CONTROL_TOKEN token = LIGHT_CONTROL_TOKEN_INVALID;
    BOOL_T can_acquire = FALSE;
    UINT32_T counter = 0;
    UINT32_T now = 0;
    
    if (priority == LIGHT_CONTROL_PRIO_NONE) {
        return token;
    }
    
    /* 获取当前时间（用于超时检测） */
    now = tal_system_get_millisecond();
    
    /* 检查是否有超时的控制（自动释放） */
    if (s_current_control_prio != LIGHT_CONTROL_PRIO_NONE && 
        s_control_start_time > 0 &&
        (now - s_control_start_time) > LIGHT_CONTROL_TIMEOUT_MS) {
        /* 超时，自动释放控制权 */
        s_current_control_prio = LIGHT_CONTROL_PRIO_NONE;
        s_current_control_owner = 0;
        s_current_token = LIGHT_CONTROL_TOKEN_INVALID;  /* 清除token */
        s_control_start_time = 0;
    }
    
    /* 快速检查是否可以获取权限 */
    if (s_current_control_prio == LIGHT_CONTROL_PRIO_NONE || 
        priority >= s_current_control_prio) {
        can_acquire = TRUE;
        counter = s_token_counter;
        s_token_counter++;
        if (s_token_counter == 0) {
            s_token_counter = 1;  /* 避免0，防止溢出 */
        }
    }
    
    /* 生成token和更新状态 */
    if (can_acquire) {
        token = make_token(owner_id, counter);
        s_current_control_prio = priority;
        s_current_control_owner = owner_id;
        s_current_token = token;  /* 保存当前有效token */
        s_control_start_time = now;  /* 记录控制开始时间 */
    }
    
    return token;
}

/* 释放控制权限（使用token） */
VOID light_control_release(LIGHT_CONTROL_TOKEN token)
{
    if (token == LIGHT_CONTROL_TOKEN_INVALID) {
        return;
    }
    
    /* 验证token */
    if (s_current_token == token) {
        /* token匹配，允许释放 */
        s_current_control_prio = LIGHT_CONTROL_PRIO_NONE;
        s_current_control_owner = 0;
        s_current_token = LIGHT_CONTROL_TOKEN_INVALID;
        s_control_start_time = 0;  /* 重置超时计时 */
    }
}

/* 获取当前控制优先级 */
LIGHT_CONTROL_PRIO_E light_control_get_current(VOID)
{
    return s_current_control_prio;
}

/* 适配1103的全局变量（g_power_on和g_light_mode） */
/* 9999使用s_light_power，这里提供适配接口 */
#define g_power_on s_light_power
#define g_light_mode LIGHT_MODE_MIXED  /* 9999应用使用混合模式 */
#define LIGHT_MODE_COLD 0
#define LIGHT_MODE_WARM 1
#define LIGHT_MODE_MIXED 2

/**
 * @brief 延迟启用实时状态保存的后台任务
 * @note 等待渐变完成后才启用保存，避免KV写入导致复位
 */
STATIC VOID __restore_save_enable_task(VOID *arg)
{
    (VOID)arg;
    /* 渐变时间最长3秒，延迟4秒确保渐变完成 */
    tal_system_sleep(4000);
    
    extern VOID dp_enable_realtime_light_save(BOOL_T enable);
    dp_enable_realtime_light_save(TRUE);
    TAL_PR_NOTICE("[RESTORE] Realtime light save enabled after fade completion");
    
    /* 延迟保存通电勿扰的上电次数和状态，避免在恢复灯光的关键路径上立即写入KV */
    extern VOID dp_dnd_power_cycle_delayed_save(VOID);
    extern VOID dp_dnd_state_delayed_save(VOID);
    dp_dnd_power_cycle_delayed_save();
    dp_dnd_state_delayed_save();
    
    s_restore_save_enable_thread = NULL;
}

STATIC VOID __restore_light_after_indicator(VOID)
{
    /* 恢复前禁用实时状态保存，避免覆盖断电前的状态 */
    extern VOID dp_enable_realtime_light_save(BOOL_T enable);
    dp_enable_realtime_light_save(FALSE);
    TAL_PR_NOTICE("[RESTORE] Realtime light save disabled before restore");
    
    /* 如果组网成功标志已设置，保持常亮，不恢复DP33（防止DP33恢复覆盖灯光设置） */
    if (s_provisioning_success_light_on) {
        TAL_PR_NOTICE("[RESTORE] Provisioning success flag is set, keeping light ON (100%%, 50%% color temp) instead of restoring DP33");
        light_control_set_state(TRUE, 1000, 500);
        dp_enable_realtime_light_save(TRUE);
        /* 清除标志，允许后续正常恢复 */
        s_provisioning_success_light_on = FALSE;
        return;
    }
    
    /* 检查通电勿扰：如果开启且count < 2，则阻止灯光恢复（优先级高于DP33）
     * 注意：__do_not_disturb_should_block_light() 直接读取当前状态，不阻塞等待
     */
    if (__do_not_disturb_should_block_light()) {
        TAL_PR_NOTICE("[RESTORE] Light restore blocked by do-not-disturb (count < 2), keeping light OFF");
        /* 保持灯关闭，但启用实时状态保存，允许其他功能正常 */
        dp_enable_realtime_light_save(TRUE);
        return;
    }
    
    /* 通电勿扰未阻止，继续执行DP33恢复逻辑 */
    extern BOOL_T dp_apply_cached_power_memory(VOID);

    if (dp_apply_cached_power_memory()) {
        TAL_PR_NOTICE("[RESTORE] Power memory applied from KV immediately");
        /* 使用后台任务延迟启用实时状态保存，等待渐变完成，避免KV写入导致复位 */
        if (s_restore_save_enable_thread == NULL) {
            THREAD_CFG_T task_cfg = {
                .thrdname   = "restore_save",
                .priority   = THREAD_PRIO_6,
                .stackDepth = 1024,
            };
            OPERATE_RET rt = tal_thread_create_and_start(&s_restore_save_enable_thread, NULL, NULL,
                                                         __restore_save_enable_task, NULL, &task_cfg);
            if (rt != OPRT_OK) {
                TAL_PR_ERR("[RESTORE] Failed to start restore save enable task, rt=%d", rt);
                /* 失败时立即启用，避免功能异常 */
                dp_enable_realtime_light_save(TRUE);
            }
        }
        return;
    }

    /* 没有DP33断电记忆时，组网成功后保持常亮（亮度100%，色温50%） */
    TAL_PR_NOTICE("[RESTORE] No power memory in KV, setting light to constant ON (100%%, 50%% color temp)");
    light_control_set_state(TRUE, 1000, 500);
    dp_enable_realtime_light_save(TRUE);
}

STATIC VOID __network_indicator_task(VOID *arg)
{
    (VOID)arg;
    UINT32_T start_ms = tal_system_get_millisecond();
    BOOL_T network_connected = FALSE;

    const UINT16_T min_brightness = 10;     /* 1% */
    const UINT16_T max_brightness = 500;    /* 50% */
    const UINT16_T indicator_color = 500;   /* 50% 色温 */
    const UINT16_T constant_brightness = 1000; /* 100% 常亮 */
    const UINT32_T breathing_duration_ms = FIRST_BOOT_BREATH_MS;
    const UINT32_T constant_duration_ms = FIRST_BOOT_TOTAL_MS - FIRST_BOOT_BREATH_MS;
    const UINT32_T breathing_cycle_ms = 3000;
    const UINT32_T breathing_step_ms = 20;

    /* 无论首次还是非首次配网，都显示配网指示器 */
    TAL_PR_NOTICE("[PROVISION] Provisioning indicator: 30s blinking (1%%~50%%) + constant 100%% when WiFi connected");
    STATIC BOOL_T wifi_connected_light_set = FALSE;  /* 标记WiFi连接后是否已设置常亮 */

    while (s_network_indicator_active) {
        /* 检查WiFi和MQTT连接状态 */
        BOOL_T wifi_connected = tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT);
        BOOL_T mqtt_connected = get_mqc_conn_stat();
        
        /* WiFi和MQTT都连接成功，配网完成 */
        if (wifi_connected && mqtt_connected) {
            if (!network_connected) {
                TAL_PR_NOTICE("[PROVISION] Network connected! Entering online mode");
                network_connected = TRUE;
                set_run_mode(RUN_MODE_ONLINE);
                TAL_PR_NOTICE("[PROVISION] Run mode set to ONLINE");
                
                /* 清除离线模式渐变标志，确保在线模式的PWM更新不被阻止 */
                extern volatile BOOL_T s_offline_fade_active;
                if (s_offline_fade_active) {
                    TAL_PR_NOTICE("[PROVISION] Clearing offline fade active flag to allow online PWM control");
                    s_offline_fade_active = FALSE;
                }
                
                /* 设置组网成功标志，防止DP33恢复覆盖灯光设置 */
                s_provisioning_success_light_on = TRUE;
                /* 在退出循环前清除配网指示器标志，避免竞态条件 */
                s_network_indicator_active = FALSE;
                break;
            }
        }
        
        /* WiFi已连接但MQTT未连接（组网时连接到手机后，即手机转圈加载时），立即切换到常亮 */
        if (wifi_connected && !mqtt_connected) {
            if (!wifi_connected_light_set) {
                TAL_PR_NOTICE("[PROVISION] ===== 组网时连接到手机后（手机转圈加载时），灯板即保持常亮 =====");
                TAL_PR_NOTICE("[PROVISION] WiFi connected (phone loading), switching to constant light (100%%, 50%% color temp)");
                __set_light_immediate(constant_brightness, indicator_color);
                wifi_connected_light_set = TRUE;
                /* 记录手机转圈加载时的灯光状态 */
                extern BOOL_T s_light_power;
                extern UINT16_T s_brightness_value;
                extern UINT16_T s_color_value;
                TAL_PR_NOTICE("[LIGHT_STATUS] Phone loading state - Power: %s, Brightness: %d%%, ColorTemp: %d%%", 
                             s_light_power ? "ON" : "OFF", s_brightness_value / 10, s_color_value / 10);
            } else {
                /* 保持常亮状态 */
                __set_light_immediate(constant_brightness, indicator_color);
            }
            tal_system_sleep(200);
            continue;  /* 跳过呼吸灯逻辑，直接进入下一轮循环 */
        }

        UINT32_T now = tal_system_get_millisecond();
        UINT32_T elapsed = now - start_ms;

        if (elapsed >= FIRST_BOOT_TOTAL_MS) {
            TAL_PR_NOTICE("[PROVISION] Provisioning timeout (%d ms), entering offline mode immediately", FIRST_BOOT_TOTAL_MS);
            
            /* 超时后立即进入离线模式并停止配网服务，使设备不被手机搜索到 */
            /* 先清除配网指示器标志，确保硬件操作不被阻止 */
            s_network_indicator_active = FALSE;
            set_run_mode(RUN_MODE_OFFLINE);
            extern VOID offline_mode_enable(BOOL_T enable);
            offline_mode_enable(TRUE);
            TAL_PR_NOTICE("[PROVISION] Offline mode enabled immediately on timeout");
            
            /* 停止配网服务 */
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
            tuya_wifi_netcfg_stop();
            /* 直接停止WiFi AP模式（热点），确保设备热点被关闭，参考：https://developer.tuya.com/cn/docs/iot-device-dev/TuyaOS-iot_abi_driver_wifi?id=Kcusut0tv85ee */
            tkl_wifi_stop_ap();
            /* 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
            tal_ble_advertising_stop();
            TAL_PR_NOTICE("[PROVISION] Provisioning service stopped due to timeout, device will not be discoverable");
            TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
            TAL_PR_NOTICE("[OFFLINE_STATUS] Offline mode enabled - Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
#endif
            
            /* 设置灯光状态并标记超时，避免任务退出后重复处理 */
            extern VOID dp_enable_realtime_light_save(BOOL_T enable);
            dp_enable_realtime_light_save(TRUE);
            __set_light_immediate(constant_brightness, indicator_color);
            network_connected = FALSE;  /* 标记为未连接，但已处理超时 */
            
            break;
        }

        /* WiFi未连接时，显示配网指示器（呼吸灯+常亮） */
        if (!wifi_connected) {
            wifi_connected_light_set = FALSE;  /* 重置标记 */
            if (elapsed < breathing_duration_ms) {
                /* 前30秒：呼吸灯效果 */
                UINT32_T cycle_pos = elapsed % breathing_cycle_ms;
                float phase = (float)cycle_pos / (float)breathing_cycle_ms;
                float sin_value = sinf(phase * 2.0f * 3.14159265358979323846f);
                float normalized = (sin_value + 1.0f) * 0.5f;
                float brightness_span = (float)(max_brightness - min_brightness);
                float brightness_value = (float)min_brightness + brightness_span * normalized;
                if (brightness_value < (float)min_brightness) {
                    brightness_value = (float)min_brightness;
                }
                if (brightness_value > (float)max_brightness) {
                    brightness_value = (float)max_brightness;
                }
                __set_light_immediate((UINT16_T)(brightness_value + 0.5f), indicator_color);
                tal_system_sleep(breathing_step_ms);
            } else if (elapsed < (breathing_duration_ms + constant_duration_ms)) {
                /* 接下来保持 100% 亮度，色温50%，直到3分钟窗口结束 */
                __set_light_immediate(constant_brightness, indicator_color);
                tal_system_sleep(200);
            } else {
                tal_system_sleep(200);
            }
        }
    }

#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
    tuya_wifi_netcfg_stop();
#endif

    if (network_connected) {
        TAL_PR_NOTICE("[PROVISION] ===== 组网成功，灯板即保持常亮 =====");
        TAL_PR_NOTICE("[PROVISION] Provisioning successful, restoring light state");
        TAL_PR_NOTICE("[PROVISION] Hardware control enabled (encoder/touchpad operations allowed)");
        
        /* 清除离线模式渐变标志，确保在线模式的PWM更新不被阻止 */
        extern volatile BOOL_T s_offline_fade_active;
        if (s_offline_fade_active) {
            TAL_PR_NOTICE("[PROVISION] Clearing offline fade active flag to allow online PWM control");
            s_offline_fade_active = FALSE;
        }
        
        /* 设置组网成功标志，防止DP33恢复覆盖灯光设置 */
        s_provisioning_success_light_on = TRUE;
        
        /* 检查是否是首次配网（解绑后的第一次连接） */
        if (!__is_first_provision_done()) {
            /* 首次配网：设置为常亮（100%亮度，50%色温） */
            TAL_PR_NOTICE("[PROVISION] First provisioning after unbind, setting light to constant ON (100%%, 50%% color temp)");
            light_control_set_state(TRUE, 1000, 500);
            __mark_first_provision_done();  /* 标记首次配网完成 */
            /* 记录配网成功时的灯光状态 */
            extern BOOL_T s_light_power;
            extern UINT16_T s_brightness_value;
            extern UINT16_T s_color_value;
            TAL_PR_NOTICE("[LIGHT_STATUS] Provisioning success (first time) - Power: %s, Brightness: %d%%, ColorTemp: %d%%", 
                         s_light_power ? "ON" : "OFF", s_brightness_value / 10, s_color_value / 10);
        } else {
            /* 后续连接：根据DP33恢复灯光（但组网成功标志会阻止DP33关闭灯光） */
            TAL_PR_NOTICE("[PROVISION] Subsequent connection, restoring light state from DP33");
            __restore_light_after_indicator();
            /* 记录配网成功时的灯光状态 */
            extern BOOL_T s_light_power;
            extern UINT16_T s_brightness_value;
            extern UINT16_T s_color_value;
            TAL_PR_NOTICE("[LIGHT_STATUS] Provisioning success (subsequent) - Power: %s, Brightness: %d%%, ColorTemp: %d%%", 
                         s_light_power ? "ON" : "OFF", s_brightness_value / 10, s_color_value / 10);
        }
    } else {
        /* 配网超时：检查是否已经在循环中处理过超时（通过检查离线模式状态） */
        extern BOOL_T offline_mode_is_enabled(VOID);
        if (!offline_mode_is_enabled()) {
            /* 如果还未进入离线模式，说明超时处理可能未完成，再次处理 */
            TAL_PR_NOTICE("[PROVISION] Provisioning timeout, entering offline mode (light kept ON)");
            /* 先清除配网指示器标志，确保硬件操作不被阻止 */
            s_network_indicator_active = FALSE;
            set_run_mode(RUN_MODE_OFFLINE);
            TAL_PR_NOTICE("[PROVISION] Run mode set to OFFLINE");
            TAL_PR_NOTICE("[PROVISION] Hardware control enabled (encoder/touchpad operations allowed)");
            extern VOID dp_enable_realtime_light_save(BOOL_T enable);
            dp_enable_realtime_light_save(TRUE);
            /* 进入离线模式时保持常亮（100%亮度，50%色温） */
            __set_light_immediate(constant_brightness, indicator_color);
            offline_mode_enable(TRUE);
            TAL_PR_NOTICE("[PROVISION] Offline mode enabled, light kept ON (100%%, 50%% color temp)");
            
            /* 离线模式下禁用配网服务，设备不可被搜索 */
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
            tuya_wifi_netcfg_stop();
            /* 直接停止WiFi AP模式（热点），确保设备热点被关闭 */
            tkl_wifi_stop_ap();
            /* 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
            tal_ble_advertising_stop();
            TAL_PR_NOTICE("[PROVISION] Provisioning service stopped, device will not be discoverable in offline mode");
            TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
            TAL_PR_NOTICE("[OFFLINE_STATUS] Offline mode enabled - Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
            TAL_PR_NOTICE("[PROVISION] User can re-enter provisioning mode via hardware reset (touchpad/encoder long press)");
#endif
        } else {
            /* 已经在循环中处理过超时，这里只需要清理标志 */
            TAL_PR_NOTICE("[PROVISION] Provisioning timeout already handled in loop, cleaning up");
            s_network_indicator_active = FALSE;
        }
    }

    s_network_indicator_active = FALSE;
    s_network_indicator_thread = NULL;
}

STATIC VOID __start_network_indicator_sequence(BOOL_T first_boot)
{
    if (s_network_indicator_active || s_network_indicator_thread != NULL) {
        return;
    }

    /* 上电后正常走配网流程，不检查离线模式（配网超时后才进入离线模式） */
    /* 如果当前处于离线模式，不要启动配网流程（离线模式下设备不应被搜索） */
    extern BOOL_T offline_mode_is_enabled(VOID);
    if (offline_mode_is_enabled()) {
        TAL_PR_NOTICE("[PROVISION] Offline mode is enabled, skipping provisioning flow (device will not be discoverable)");
        return;
    }

    NETWORK_STATUS_E net_status = tuya_svc_netmgr_get_status();
    if (net_status == NETWORK_STATUS_MQTT) {
        TAL_PR_NOTICE("[PROVISION] MQTT already connected, entering online mode directly");
        set_run_mode(RUN_MODE_ONLINE);
        offline_mode_enable(FALSE);
        
        /* 清除离线模式渐变标志，确保在线模式的PWM更新不被阻止 */
        extern volatile BOOL_T s_offline_fade_active;
        if (s_offline_fade_active) {
            TAL_PR_NOTICE("[PROVISION] Clearing offline fade active flag to allow online PWM control");
            s_offline_fade_active = FALSE;
        }
        
        /* 设置组网成功标志，防止DP33恢复覆盖灯光设置 */
        s_provisioning_success_light_on = TRUE;
        
        /* 检查是否是首次配网（解绑后的第一次连接） */
        if (!__is_first_provision_done()) {
            /* 首次配网：设置为常亮（100%亮度，50%色温） */
            TAL_PR_NOTICE("[PROVISION] First provisioning after unbind (MQTT already connected), setting light to constant ON (100%%, 50%% color temp)");
            light_control_set_state(TRUE, 1000, 500);
            __mark_first_provision_done();  /* 标记首次配网完成 */
        } else {
            /* 后续连接：根据DP33恢复灯光（但组网成功标志会阻止DP33关闭灯光） */
            TAL_PR_NOTICE("[PROVISION] Subsequent connection (MQTT already connected), restoring light state from DP33");
            __restore_light_after_indicator();
        }
        
        /* 启动云守护任务（检查30秒内是否保持连接）
         * 注意：如果MQTT已连接，云守护任务会立即退出，不会产生冲突
         */
        __start_cloud_guard_task();
        return;
    }

    if (net_status != NETWORK_STATUS_OFFLINE && net_status != NETWORK_STATUS_LOCAL) {
        TAL_PR_DEBUG("[PROVISION] Network status is %d, not starting provisioning", net_status);
        return;
    }

    TAL_PR_NOTICE("[PROVISION] Starting provisioning flow (network status: %d)", net_status);
    offline_mode_enable(FALSE);
    TAL_PR_NOTICE("[PROVISION] Offline mode disabled during provisioning");
    TAL_PR_NOTICE("[PROVISION] Hardware control disabled during provisioning (encoder/touchpad operations will be ignored)");

    s_indicator_prev_power = s_light_power;
    s_indicator_prev_brightness = s_brightness_value;
    s_indicator_prev_color = s_color_value;

    s_network_indicator_active = TRUE;

    THREAD_CFG_T indicator_cfg = {
        .thrdname   = "net_indicator",
        .priority   = THREAD_PRIO_6,
        .stackDepth = 2048,
    };

    OPERATE_RET rt = tal_thread_create_and_start(&s_network_indicator_thread, NULL, NULL, __network_indicator_task, NULL, &indicator_cfg);
    if (rt != OPRT_OK) {
        TAL_PR_ERR("Failed to start network indicator thread, rt=%d", rt);
        s_network_indicator_active = FALSE;
        s_network_indicator_thread = NULL;
        __restore_light_after_indicator();
    }
}

STATIC VOID __cloud_guard_task(VOID *arg)
{
    (VOID)arg;
    UINT32_T elapsed = 0;

    while (!s_cloud_guard_stop && elapsed < POST_BOOT_GUARD_MS) {
        if (tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT) && get_mqc_conn_stat()) {
            TAL_PR_NOTICE("[BOOT_FLOW] Cloud connected within guard window");
            s_cloud_guard_stop = TRUE;
            break;
        }
        tal_system_sleep(200);
        elapsed += 200;
    }

    if (!s_cloud_guard_stop) {
        TAL_PR_NOTICE("[BOOT_FLOW] Cloud connect timeout (%d ms), entering offline mode", POST_BOOT_GUARD_MS);
        set_run_mode(RUN_MODE_OFFLINE);
        offline_mode_enable(TRUE);
        
        /* 停止配网服务，确保设备在离线模式下不可被手机搜索到 */
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
        tuya_wifi_netcfg_stop();
        /* 直接停止WiFi AP模式（热点），确保设备热点被关闭 */
        tkl_wifi_stop_ap();
        /* 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
        tal_ble_advertising_stop();
        TAL_PR_NOTICE("[BOOT_FLOW] Provisioning service stopped, device will not be discoverable in offline mode");
        TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
        TAL_PR_NOTICE("[OFFLINE_STATUS] Cloud guard timeout - Offline mode enabled, Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
#endif
        
        /* 云守护任务超时后保持常亮，不关闭灯光（与组网成功后保持常亮的需求一致） */
        TAL_PR_NOTICE("[BOOT_FLOW] Keeping light ON (100%%, 50%% color temp) after cloud guard timeout");
        light_control_set_state(TRUE, 1000, 500);
    }

    s_cloud_guard_stop = TRUE;
    s_cloud_guard_thread = NULL;
}

STATIC VOID __start_cloud_guard_task(VOID)
{
    if (s_cloud_guard_thread != NULL) {
        return;
    }

    s_cloud_guard_stop = FALSE;
    THREAD_CFG_T guard_cfg = {
        .thrdname   = "cloud_guard",
        .priority   = THREAD_PRIO_6,
        .stackDepth = 2048,
    };

    OPERATE_RET rt = tal_thread_create_and_start(&s_cloud_guard_thread, NULL, NULL, __cloud_guard_task, NULL, &guard_cfg);
    if (rt != OPRT_OK) {
        TAL_PR_ERR("Failed to start cloud guard task, rt=%d", rt);
        s_cloud_guard_thread = NULL;
        s_cloud_guard_stop = TRUE;
    }
}

/**
 * @brief 检查通电勿扰是否应该阻止灯光恢复
 * @return TRUE=应该阻止灯光恢复（count < 2），FALSE=允许灯光恢复
 * @note 此函数只检查是否应该阻止灯光，不阻止其他初始化
 */
STATIC BOOL_T __do_not_disturb_should_block_light(VOID)
{
    extern BOOL_T dp_get_do_not_disturb_state(VOID);
    extern BOOL_T dp_check_do_not_disturb_power_cycle(VOID);

    /* 非阻塞检查：直接读取当前DP34状态，不等待云端同步
     * 避免长时间阻塞导致看门狗复位
     * 如果云端状态尚未同步，使用KV中保存的本地状态 */
    BOOL_T do_not_disturb = dp_get_do_not_disturb_state();
    TAL_PR_NOTICE("[DO_NOT_DISTURB] Current state: %s", do_not_disturb ? "ENABLED" : "DISABLED");

    if (do_not_disturb) {
        if (dp_check_do_not_disturb_power_cycle()) {
            /* count < 2，应该阻止灯光恢复，但不阻止其他初始化 */
            TAL_PR_NOTICE("[DO_NOT_DISTURB] Power cycle count insufficient (count < 2), blocking light restore only");
            return TRUE;
        } else {
            /* count >= 2，允许灯光恢复 */
            TAL_PR_NOTICE("[DO_NOT_DISTURB] Power cycle count sufficient (count >= 2), allowing light restore");
        }
    } else {
        TAL_PR_NOTICE("[DO_NOT_DISTURB] Do not disturb disabled, allowing light restore");
    }

    return FALSE;
}

STATIC VOID __first_boot_flow(VOID)
{
    TAL_PR_NOTICE("[BOOT] First boot - starting provisioning");
    /* 上电后保持在线模式，正常走配网流程（配网成功则进入在线，否则进入离线） */
    s_current_run_mode = RUN_MODE_ONLINE;
    light_control_set(FALSE);
    __start_network_indicator_sequence(TRUE);
}


// output qrcode when ENABLE_QRCODE_ACTIVE == 1 && ENABLE_WIFI_QRCODE == 0
#if (defined(ENABLE_QRCODE_ACTIVE) && (ENABLE_QRCODE_ACTIVE == 1)) && (!(defined(ENABLE_WIFI_QRCODE) && (ENABLE_WIFI_QRCODE == 1)))
// qrcode打印
extern INT_T qrcode_exec(INT_T argc, CHAR_T **argv);
STATIC INT_T __qrcode_printf(CHAR_T *msg)
{
    CHAR_T *qrcode_argv[] = {
        "qrcode_exec", "-m", "3", "-t", "ansiutf8", msg
    };

    return qrcode_exec(sizeof(qrcode_argv)/sizeof(qrcode_argv[0]), qrcode_argv);
}

// TuyaOS获取到短链接之后调用此接口输出qrcode打印
STATIC VOID __qrcode_active_shourturl_cb(CONST CHAR_T *shorturl)
{
    if (NULL == shorturl) {
        return;
    }

    TAL_PR_DEBUG("shorturl : %s", shorturl);
    ty_cJSON *item = ty_cJSON_Parse(shorturl);
    __qrcode_printf(ty_cJSON_GetObjectItem(item, "shortUrl")->valuestring);
    ty_cJSON_Delete(item);

    return;
}
#endif

/**
 * @brief SOC device upgrade entry
 *
 * @param[in] fw: firmware info
 *
 * @return OPRT_OK on success. Others on error, please refer to "tuya_error_code.h".
 */
STATIC OPERATE_RET __soc_dev_rev_upgrade_info_cb(IN CONST FW_UG_S *fw)
{
    TAL_PR_DEBUG("SOC Rev Upgrade Info");
    TAL_PR_DEBUG("fw->tp:%d", fw->tp);
    TAL_PR_DEBUG("fw->fw_url:%s", fw->fw_url);
    TAL_PR_DEBUG("fw->fw_hmac:%s", fw->fw_hmac);
    TAL_PR_DEBUG("fw->sw_ver:%s", fw->sw_ver);
    TAL_PR_DEBUG("fw->file_size:%u", fw->file_size);

    return OPRT_OK;
}

/**
 * @brief SOC device cloud state change callback
 *
 * @param[in] status: current status
 *
 * @return none
 */
STATIC VOID_T __soc_dev_status_changed_cb(IN CONST GW_STATUS_E status)
{
    TAL_PR_DEBUG("SOC TUYA-Cloud Status:%d", status);
    return;
}


/**
 * @brief SOC device DP query entry
 *
 * @param[in] dp_qry: DP query list
 *
 * @return none
 */
STATIC VOID_T __soc_dev_dp_query_cb(IN CONST TY_DP_QUERY_S *dp_qry)
{
    UINT32_T index = 0;

    TAL_PR_DEBUG("SOC Rev DP Query Cmd");
    if (dp_qry->cid != NULL) {
        TAL_PR_ERR("soc not have cid.%s", dp_qry->cid);
    }

    if (dp_qry->cnt == 0) {
        TAL_PR_DEBUG("soc rev all dp query");
        respone_device_all_status();
    } else {
        TAL_PR_DEBUG("soc rev dp query cnt:%d", dp_qry->cnt);
        for (index = 0; index < dp_qry->cnt; index++) {
            TAL_PR_DEBUG("rev dp query:%d", dp_qry->dpid[index]);
            // UserTODO
        }
    }

    return;
}

/**
 * @brief SOC device format command data delivery entry
 *
 * @param[in] dp: obj dp info
 *
 * @return none
 */
STATIC VOID_T __soc_dev_obj_dp_cmd_cb(IN CONST TY_RECV_OBJ_DP_S *dp)
{

    TAL_PR_DEBUG("SOC Rev DP Obj Cmd t1:%d t2:%d CNT:%u", dp->cmd_tp, dp->dtt_tp, dp->dps_cnt);

    dp_obj_process(dp->dps, dp->dps_cnt);

    return;
}

/**
 * @brief SOC device transparently transmits command data delivery entry
 *
 * @param[in] dp: raw dp info
 *
 * @return none
 */
STATIC VOID_T __soc_dev_raw_dp_cmd_cb(IN CONST TY_RECV_RAW_DP_S *dp)
{
    if (dp == NULL) {
        TAL_PR_ERR("RAW DP callback received NULL pointer!");
        return;
    }

    /* 简化日志输出，避免缓冲区溢出 */
    TAL_PR_NOTICE("[CB] RAW_DP: dpid=%d len=%u", dp->dpid, dp->len);
    
    if (dp->data == NULL) {
        TAL_PR_ERR("[CB] RAW DP data is NULL!");
        return;
    }
    
    if (dp->len == 0) {
        TAL_PR_WARN("[CB] RAW DP data length is 0!");
        return;
    }

    dp_raw_process(dp->dpid, dp->data, dp->len);
    TAL_PR_NOTICE("[CB] RAW_DP RETURNED");

    return;
}

/**
 * @brief  app process when device reset 
 *
 * @param[in] type: gateway reset type
 *
 * @return none
 */
STATIC VOID_T __soc_dev_reset_inform_cb(GW_RESET_TYPE_E type)
{
    TAL_PR_NOTICE("[RESET] Reset callback triggered, type=%d", type);
    
    /* 复位回调函数，由 tuya_iot_wf_gw_unactive() 自动调用 */
    /* 此回调会在设备解绑后触发，用于清理应用层数据 */
    
    /* 清除首次配网完成标记，下次连接时识别为首次配网 */
    __clear_first_provision_done();

    return;
}

/**
 * @brief SOC external network status change callback
 * 
 * @param[in/out] data 
 * @return STATIC 
 */
/**
 * @brief 解绑检测任务：检测WIFI已连接但MQTT未连接，30秒后清除WIFI配置
 */
STATIC VOID __unbind_detect_task(VOID *arg)
{
    (VOID)arg;
    
    TAL_PR_NOTICE("[UNBIND] Unbind detect task started, waiting %d seconds...", UNBIND_DETECT_TIMEOUT_MS / 1000);
    
    while (s_unbind_detect_active) {
        tal_system_sleep(1000);  /* 每秒检查一次 */
        
        if (!s_unbind_detect_active) {
            break;
        }
        
        UINT32_T now = tal_system_get_millisecond();
        UINT32_T elapsed = now - s_unbind_detect_start_ms;
        
        /* 检查是否超时 */
        if (elapsed >= UNBIND_DETECT_TIMEOUT_MS) {
            /* 再次确认：WIFI已连接但MQTT未连接 */
            if (tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT) && !get_mqc_conn_stat()) {
                TAL_PR_NOTICE("[UNBIND] Timeout detected: WIFI connected but MQTT not connected for %d seconds", 
                             UNBIND_DETECT_TIMEOUT_MS / 1000);
                
                /* 检查是否已进入离线模式，如果已进入离线模式，不清除WiFi配置，直接停止配网服务 */
                extern BOOL_T offline_mode_is_enabled(VOID);
                if (offline_mode_is_enabled()) {
                    TAL_PR_NOTICE("[UNBIND] Offline mode is enabled, stopping provisioning service instead of clearing WiFi");
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
                    extern OPERATE_RET tuya_wifi_netcfg_stop(VOID);
                    tuya_wifi_netcfg_stop();
                    /* 直接停止WiFi AP模式（热点），确保设备热点被关闭 */
                    tkl_wifi_stop_ap();
                    /* 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
                    tal_ble_advertising_stop();
                    TAL_PR_NOTICE("[UNBIND] Provisioning service stopped, device will not be discoverable");
                    TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
                    TAL_PR_NOTICE("[OFFLINE_STATUS] Unbind detect timeout - Offline mode already enabled, Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
#endif
                } else {
                    TAL_PR_NOTICE("[UNBIND] Clearing WIFI configuration to enter provisioning mode...");
                    
                    /* 清除WIFI配置，让设备重新进入配网模式 */
                    OPERATE_RET ret = tuya_svc_netmgr_reset(GW_LOCAL_RESET_FACTORY);
                    if (ret == OPRT_OK) {
                        TAL_PR_NOTICE("[UNBIND] WIFI configuration cleared successfully, device will enter provisioning mode");
                    } else {
                        TAL_PR_ERR("[UNBIND] Failed to clear WIFI configuration, ret=%d", ret);
                    }
                }
            } else {
                TAL_PR_NOTICE("[UNBIND] Status changed during timeout, canceling unbind detect");
            }
            
            s_unbind_detect_active = FALSE;
            break;
        }
        
        /* 检查MQTT是否已连接（提前退出） */
        if (get_mqc_conn_stat()) {
            TAL_PR_NOTICE("[UNBIND] MQTT connected, canceling unbind detect");
            s_unbind_detect_active = FALSE;
            break;
        }
        
        /* 检查WIFI是否断开（提前退出） */
        if (!tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT)) {
            TAL_PR_NOTICE("[UNBIND] WIFI disconnected, canceling unbind detect");
            s_unbind_detect_active = FALSE;
            break;
        }
    }
    
    TAL_PR_NOTICE("[UNBIND] Unbind detect task stopped");
    
    /* 清理线程句柄，允许下次重新创建 */
    s_unbind_detect_thread = NULL;
    return;
}

STATIC OPERATE_RET __soc_dev_net_status_cb(VOID *data)
{
    STATIC BOOL_T s_syn_all_status = FALSE;
    STATIC BOOL_T s_first_network_connect = TRUE;

    TAL_PR_DEBUG("network status changed!");
    if (tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT)) {
        TAL_PR_DEBUG("linkage status changed, current status is up");
        if (get_mqc_conn_stat()) {
            TAL_PR_DEBUG("mqtt is connected!");
            s_cloud_guard_stop = TRUE;

            /* MQTT已连接，停止解绑检测 */
            if (s_unbind_detect_active) {
                TAL_PR_NOTICE("[UNBIND] MQTT connected, stopping unbind detect");
                s_unbind_detect_active = FALSE;
            }
            
            /* 通知断电复位模块：配网成功，开始计时 */
            extern VOID __offline_notify_provisioned(VOID);
            __offline_notify_provisioned();
            
            /* 如果当前是离线模式，配网成功后切换到在线模式 */
            if (s_current_run_mode == RUN_MODE_OFFLINE) {
                TAL_PR_NOTICE("[NETWORK] MQTT connected in offline mode, switching to online mode");
                set_run_mode(RUN_MODE_ONLINE);
                offline_mode_enable(FALSE);
                TAL_PR_NOTICE("[NETWORK] Run mode switched to ONLINE, offline mode disabled");
            }
            
            /* 标记已连接过MQTT（用于区分首次配网和解绑） */
            if (!s_has_connected_mqtt_before) {
                s_has_connected_mqtt_before = TRUE;
                TAL_PR_NOTICE("[UNBIND] First MQTT connection, marking as connected before");
            }

            if (FALSE == s_syn_all_status) {
                upload_device_all_status();
                s_syn_all_status = TRUE;
                
                /* 网络连接后，保存通电勿扰状态（如果标记需要保存） */
                extern VOID dp_dnd_state_delayed_save(VOID);
                dp_dnd_state_delayed_save();
            }
            
            /* 首次配网成功标记（不再应用默认灯光，等待DP33/DP恢复） */
            if (s_first_network_connect) {
                TAL_PR_NOTICE("[NETWORK] First network connection, waiting for DP33/DP restore");
                s_first_network_connect = FALSE;
            }
            
            /* 确保组网成功后灯光保持常亮（亮度100%，色温50%） */
            /* 检查当前灯光状态，如果未设置组网成功标志或灯光未开启，强制设置为常亮 */
            if (!s_provisioning_success_light_on || !light_control_get()) {
                TAL_PR_NOTICE("[NETWORK] MQTT connected, ensuring light is ON (100%%, 50%% color temp)");
                s_provisioning_success_light_on = TRUE;
                light_control_set_state(TRUE, 1000, 500);
            }
        } else {
            /* WIFI已连接但MQTT未连接 */
            TAL_PR_DEBUG("WIFI connected but MQTT not connected");
            
            /* 如果处于离线模式，需要判断WiFi连接是配网完成还是自动连接 */
            extern BOOL_T offline_mode_is_enabled(VOID);
            if (offline_mode_is_enabled()) {
                /* 检查是否是配网完成（配网指示器曾经激活过，或者首次配网） */
                BOOL_T is_provisioning_completed = FALSE;
                
                /* 情况1：首次配网（之前从未连接过MQTT），WiFi连接成功视为配网完成 */
                if (!s_has_connected_mqtt_before) {
                    is_provisioning_completed = TRUE;
                    TAL_PR_NOTICE("[NETWORK] First provisioning: WiFi connected but MQTT not connected in offline mode, provisioning completed");
                }
                /* 情况2：配网指示器曾经激活过，说明正在进行配网流程，WiFi连接成功视为配网完成 */
                else if (s_network_indicator_active || s_provisioning_success_light_on) {
                    is_provisioning_completed = TRUE;
                    TAL_PR_NOTICE("[NETWORK] Provisioning indicator was active: WiFi connected but MQTT not connected in offline mode, provisioning completed");
                }
                /* 情况3：其他情况（可能是WiFi自动连接），不视为配网完成，保持在离线模式 */
                else {
                    TAL_PR_NOTICE("[NETWORK] WiFi auto-connected in offline mode (not provisioning), staying in offline mode");
                    TAL_PR_NOTICE("[NETWORK] This may be due to saved WiFi credentials, not a new provisioning");
                }
                
                if (is_provisioning_completed) {
                    TAL_PR_NOTICE("[NETWORK] Provisioning completed, exiting offline mode");
                    
                    /* WiFi连接成功，说明配网已完成，退出离线模式，允许MQTT连接 */
                    if (s_current_run_mode == RUN_MODE_OFFLINE) {
                        TAL_PR_NOTICE("[NETWORK] WiFi connected after provisioning, switching to online mode");
                        set_run_mode(RUN_MODE_ONLINE);
                        offline_mode_enable(FALSE);
                        TAL_PR_NOTICE("[NETWORK] Run mode switched to ONLINE, offline mode disabled, allowing MQTT connection");
                    }
                    
                    /* 确保配网服务被停止，设备不可被搜索 */
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
                    extern OPERATE_RET tuya_wifi_netcfg_stop(VOID);
                    tuya_wifi_netcfg_stop();
                    /* 直接停止WiFi AP模式（热点），确保设备热点被关闭 */
                    tkl_wifi_stop_ap();
                    /* 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
                    tal_ble_advertising_stop();
                    TAL_PR_NOTICE("[NETWORK] Provisioning service stopped, device will not be discoverable");
                    TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
                    TAL_PR_NOTICE("[OFFLINE_STATUS] WiFi connected (MQTT not) - Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
#endif
                    /* 退出离线模式后，继续执行后续逻辑（如解绑检测） */
                } else {
                    /* WiFi自动连接，保持在离线模式，确保配网服务被停止 */
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
                    extern OPERATE_RET tuya_wifi_netcfg_stop(VOID);
                    tuya_wifi_netcfg_stop();
                    tkl_wifi_stop_ap();
                    tal_ble_advertising_stop();
                    TAL_PR_NOTICE("[NETWORK] WiFi auto-connected in offline mode, provisioning service stopped, staying in offline mode");
#endif
                }
            }
            
            /* 如果之前连接过MQTT，说明可能是解绑状态，启动解绑检测 */
            if (s_has_connected_mqtt_before && !s_unbind_detect_active) {
                TAL_PR_NOTICE("[UNBIND] WIFI connected but MQTT not connected (previously connected), starting unbind detect");
                s_unbind_detect_active = TRUE;
                s_unbind_detect_start_ms = tal_system_get_millisecond();
                
                /* 启动解绑检测线程（如果线程不存在或已结束） */
                if (s_unbind_detect_thread == NULL) {
                    THREAD_CFG_T detect_cfg = {
                        .thrdname   = "unbind_detect",
                        .priority   = THREAD_PRIO_6,
                        .stackDepth = 2048,
                    };
                    
                    OPERATE_RET rt = tal_thread_create_and_start(&s_unbind_detect_thread, NULL, NULL, 
                                                                 __unbind_detect_task, NULL, &detect_cfg);
                    if (rt != OPRT_OK) {
                        TAL_PR_ERR("[UNBIND] Failed to start unbind detect thread, rt=%d", rt);
                        s_unbind_detect_active = FALSE;
                        s_unbind_detect_thread = NULL;
                    } else {
                        TAL_PR_NOTICE("[UNBIND] Unbind detect thread started successfully");
                    }
                }
            } else if (!s_has_connected_mqtt_before) {
                /* 首次配网，WIFI已连接但MQTT未连接是正常情况，不启动解绑检测 */
                TAL_PR_DEBUG("[UNBIND] First provisioning, WIFI connected but MQTT not connected yet (normal)");
            }
        }
    } else {
        TAL_PR_DEBUG("linkage status changed, current status is down");

        /* WIFI断开，停止解绑检测 */
        if (s_unbind_detect_active) {
            TAL_PR_NOTICE("[UNBIND] WIFI disconnected, stopping unbind detect");
            s_unbind_detect_active = FALSE;
        }
        
        /* 如果处于离线模式，确保配网服务被停止，设备不可被搜索 */
        extern BOOL_T offline_mode_is_enabled(VOID);
        if (offline_mode_is_enabled()) {
            TAL_PR_NOTICE("[NETWORK] WiFi disconnected in offline mode, ensuring provisioning service is stopped");
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
            extern OPERATE_RET tuya_wifi_netcfg_stop(VOID);
            tuya_wifi_netcfg_stop();
            /* 直接停止WiFi AP模式（热点），确保设备热点被关闭 */
            tkl_wifi_stop_ap();
            /* 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
            tal_ble_advertising_stop();
            TAL_PR_NOTICE("[NETWORK] Provisioning service stopped in offline mode, device will not be discoverable");
            TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
            TAL_PR_NOTICE("[OFFLINE_STATUS] WiFi disconnected - Offline mode active, Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
#endif
        }
    }

    return OPRT_OK;
}

/**
 * @brief SOC device initialization
 *
 * @param[in] none
 *
 * @return OPRT_OK on success. Others on error, please refer to "tuya_error_code.h".
 */
OPERATE_RET __soc_device_init(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;

    ty_subscribe_event(EVENT_LINK_UP, "app1102", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    ty_subscribe_event(EVENT_LINK_DOWN, "app1102", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    ty_subscribe_event(EVENT_MQTT_CONNECTED, "app1102", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    
#if (defined(UUID) && defined(AUTHKEY))
#ifndef ENABLE_KV_FILE
    ws_db_init_mf();
#endif
    TAL_PR_NOTICE("[AUTH] Using built-in UUID/AUTHKEY from source code (lab/debug only)");
    TAL_PR_NOTICE("[AUTH] PID=%s UUID=%s", PID, UUID);
    TAL_PR_NOTICE("[AUTH] WARNING: OTA with built-in credentials will overwrite device authorization!");
    /* Set authorization information
     * Note that if you use the default authorization information of the code, there may be problems of multiple users and conflicts, 
     * so try to use all the authorizations purchased from the tuya iot platform.
     * Buying guide: https://developer.tuya.com/cn/docs/iot/lisence-management?id=Kb4qlem97idl0.
     * You can also apply for two authorization codes for free in the five-step hardware development stage of the Tuya IoT platform.
     * Authorization information can also be written through the production testing tool.
     * When the production testing function is started and the authorization is burned with the Tuya Cloud module tool, 
     * please comment out this piece of code.
     */
#ifdef ENABLE_WIFI_SERVICE
    WF_GW_PROD_INFO_S prod_info = {UUID, AUTHKEY};
    TUYA_CALL_ERR_RETURN(tuya_iot_set_wf_gw_prod_info(&prod_info));
#else
    GW_PROD_INFO_S prod_info = {UUID, AUTHKEY};
    TUYA_CALL_ERR_RETURN(tuya_iot_set_gw_prod_info(&prod_info));
#endif

#else
    TAL_PR_NOTICE("[AUTH] UUID/AUTHKEY not defined in firmware, expecting burned authorization data");
#ifndef ENABLE_KV_FILE
    TAL_PR_NOTICE("[AUTH] Calling ws_db_init_mf() to load MF credentials from flash");
    ws_db_init_mf();
    TAL_PR_NOTICE("[AUTH] ws_db_init_mf() completed");
#endif
    if (tal_system_get_reset_reason(NULL) == 0) {
        TAL_PR_NOTICE("[AUTH] Ensure the production tool has burned unique credentials before testing");
    }
    TAL_PR_NOTICE("[AUTH] TuyaOS will fetch UUID/AUTHKEY from MF storage during tuya_iot_wf_soc_dev_init");
#endif

    /* Initialize TuyaOS product information */
    TY_IOT_CBS_S iot_cbs = {0};
    iot_cbs.gw_status_cb    = __soc_dev_status_changed_cb;
    iot_cbs.gw_ug_cb        = __soc_dev_rev_upgrade_info_cb;
    iot_cbs.gw_reset_cb     = __soc_dev_reset_inform_cb;
    iot_cbs.dev_obj_dp_cb   = __soc_dev_obj_dp_cmd_cb;
    iot_cbs.dev_raw_dp_cb   = __soc_dev_raw_dp_cmd_cb;  /* RAW DP回调已注册 */
    iot_cbs.dev_dp_query_cb = __soc_dev_dp_query_cb;
    
    TAL_PR_NOTICE("IoT callbacks registered: obj_dp=%p raw_dp=%p query=%p", 
                  (VOID*)iot_cbs.dev_obj_dp_cb, 
                  (VOID*)iot_cbs.dev_raw_dp_cb, 
                  (VOID*)iot_cbs.dev_dp_query_cb);

#if (defined(ENABLE_QRCODE_ACTIVE) && (ENABLE_QRCODE_ACTIVE == 1)) && (!(defined(ENABLE_WIFI_QRCODE) && (ENABLE_WIFI_QRCODE == 1)))
    iot_cbs.active_shorturl = __qrcode_active_shourturl_cb;
#endif

#ifdef ENABLE_WIFI_SERVICE
    tuya_iot_wf_timeout_set(180);
    TAL_PR_NOTICE("[NETCFG] tuya_iot_wf_soc_dev_init start, PID=%s, SW_VER=%s", PID, USER_SW_VER);
    TUYA_CALL_ERR_RETURN(tuya_iot_wf_soc_dev_init(GWCM_OLD, WF_START_AP_FIRST, &iot_cbs, PID, USER_SW_VER));
    TAL_PR_NOTICE("[NETCFG] tuya_iot_wf_soc_dev_init success");
#ifdef ENABLE_WIRED
    // init wired linkage
    TUYA_CALL_ERR_RETURN(tuya_svc_wired_init());
#endif
#else
    TUYA_CALL_ERR_RETURN(tuya_iot_soc_init(&iot_cbs, PID, USER_SW_VER));
#endif

#ifdef ENABLE_BT_SERVICE
    tuya_ble_enable_debug(false);
#endif

    return 0;
}

STATIC VOID_T user_main(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    
    /* 注意：通电勿扰检查已移至 DP 状态恢复回调中执行
     * 因为 TuyaOS 框架会在启动后自动恢复 DP 状态，并通过回调函数通知应用
     * 这样可以确保在检查时已经获取到正确的 DP 34 状态
     */

    /* Initialization, because DB initialization takes a long time, 
     * which affects the startup efficiency of some devices, 
     * so special processing is performed during initialization to delay initialization of DB
     */
#if OPERATING_SYSTEM == SYSTEM_LINUX    
    rt= system("mkdir -p ./tuya_db_files/");
    TUYA_CALL_ERR_LOG(tuya_iot_init_params("./tuya_db_files/", NULL));
#else
    TY_INIT_PARAMS_S init_param = {0};
    init_param.init_db = TRUE;
    strcpy(init_param.sys_env, TARGET_PLATFORM);
    TUYA_CALL_ERR_LOG(tuya_iot_init_params(NULL, &init_param));
#endif

    TAL_PR_NOTICE("Firmware: %s v%s", APP_BIN_NAME, USER_SW_VER);
    TAL_PR_NOTICE("Reset reason: %d", tal_system_get_reset_reason(NULL));

    tal_log_set_manage_attr(TAL_LOG_LEVEL_DEBUG);

    /* Initialization device */
    TUYA_CALL_ERR_LOG(__soc_device_init());

    /* 检查断电复位（必须在配网系统初始化之后执行，否则解绑会失败） */
    extern VOID __offline_check_power_cycle_reset(VOID);
    __offline_check_power_cycle_reset();

    __light_pwm_init();
    
    /* 从KV加载渐变时间（断电后恢复） */
    extern VOID __fade_gradient_load_from_kv(VOID);
    __fade_gradient_load_from_kv();
    
    /* 初始化时禁用实时状态保存，避免在恢复前覆盖断电前的状态 */
    extern VOID dp_enable_realtime_light_save(BOOL_T enable);
    dp_enable_realtime_light_save(FALSE);
    
    /* 统一走首次启动流程（boot_state KV已移除） */
    __first_boot_flow();

    return;
}

/**
* @brief  task thread
*
* @param[in] arg:Parameters when creating a task
* @return none
*/
STATIC VOID_T tuya_app_thread(VOID_T *arg)
{
    /* Initialization LWIP first!!! */
#if defined(ENABLE_LWIP) && (ENABLE_LWIP == 1)
    TUYA_LwIP_Init();
#endif

    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

/**
 * @brief user entry function
 *
 * @param[in] none: 
 *
 * @return none
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
INT_T main(INT_T argc, CHAR_T **argv)
#else
VOID_T tuya_app_main(VOID)
#endif
{
    /* 初始化离线模式（编码器、触摸板、灯光控制） */
    offline_mode_init();
    
    THREAD_CFG_T thrd_param = {4096, 4, "tuya_app_main"};
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
#if OPERATING_SYSTEM == SYSTEM_LINUX
    while (1) {
        tal_system_sleep(1000);
    }
#endif
}
