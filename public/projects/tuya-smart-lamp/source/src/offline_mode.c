/**
 * @file offline_mode.c
 * @brief 离线模式模块 - 整合编码器、触摸板、灯光控制功能
 *
 * 功能：
 *  - P7 -> 冷光 PWM
 *  - P8 -> 暖光 PWM
 *  - 频率 10 kHz，占空比 0~15%
 *  - GPIO22 -> 触摸板输入
 *  - GPIO15/17/9 -> 编码器控制
 */

#include <math.h>
#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "tkl_gpio.h"
#include "tkl_pwm.h"
#include "tkl_wifi.h"
#include "tuya_iot_com_api.h"
#include "tuya_svc_netmgr.h"
#include "tuya_ws_db.h"
#include "mqc_app.h"
#include "tuya_iot_wifi_api.h"
#include "tal_bluetooth.h"
#include "offline_mode.h"
#include "encoder.h"
#include "touch.h"

/* 配网状态检查函数声明 */
extern BOOL_T light_control_network_indicator_active(VOID);

/* 在线模式灯光控制函数声明 */
extern VOID light_control_set(BOOL_T on);
extern VOID light_control_set_brightness(UINT16_T value);
extern VOID light_control_set_color_temp(UINT16_T value);
extern VOID light_control_set_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);
extern BOOL_T light_control_get(VOID);
extern UINT16_T light_control_get_brightness(VOID);
extern UINT16_T light_control_get_color_temp(VOID);

/* 状态上报函数声明 */
extern VOID upload_device_all_status(VOID_T);
/* 在线模式：标记编码器调光/调色温，用于自适应渐变 */
extern VOID light_control_set_encoder_dim_flag(BOOL_T enable);
extern VOID light_control_set_encoder_dim_duration(UINT32_T duration_ms);  /* 设置编码器渐变时长 */

/* 在线模式渐变状态变量声明（用于停止在线模式渐变） */
extern volatile BOOL_T s_fade_active;

/* 在线模式PWM值声明（用于显示PWM变化） */
extern volatile UINT16_T s_current_duty_cold;
extern volatile UINT16_T s_current_duty_warm;

/* 在线模式状态变量声明（用于更新状态） */
extern BOOL_T s_light_power;
extern UINT16_T s_brightness_value;
extern UINT16_T s_color_value;

/* PWM 配置 */
#define PWM_COLD_CHANNEL               PWM_NUM_1      /* P7 -> 冷光 */
#define PWM_WARM_CHANNEL               PWM_NUM_2      /* P8 -> 暖光 */
#define PWM_FREQUENCY_HZ               10000
#define PWM_DUTY_MAX                   1500           /* 0~1500 => 0~15% */

/* 触摸板配置 */
#define ENABLE_TOUCH_CONTROL           1
#define TOUCH_PIN                      GPIO_NUM_22    /* 触摸板输出 */
#define TOUCH_DEBOUNCE_MS              30
#define TOUCH_DOUBLE_CLICK_MS          350
#define TOUCH_POLL_INTERVAL_MS         10
#define TOUCH_LONG_PRESS_THRESHOLD_MS  600            /* 长按阈值 600ms */
#define TOUCH_LONG_PRESS_DIM_INTERVAL_MS 50          /* 长按变暗间隔 50ms */
#define TOUCH_SECOND_HOLD_MS           2000           /* 第二次点击按住阈值 2s（双击确认用） */
#define TOUCH_RESET_THRESHOLD_MS       10000          /* 触摸板复位阈值 10秒 */
#define ENCODER_RESET_THRESHOLD_MS     5000           /* 编码器复位阈值 5秒 */
#define POWER_CYCLE_RESET_COUNT        4             /* 断电复位次数 */
#define POWER_CYCLE_WINDOW_MS          60000         /* 计数窗口：1分钟（60000毫秒） */
#define POWER_CYCLE_CHECK_INTERVAL_MS  10000         /* 定时器检查间隔：10秒 */
#define KV_KEY_POWER_CYCLE_COUNT       "power_cycle"  /* KV存储键名 */

/* 编码器配置 */
#define ENABLE_ENCODER_CONTROL         1
#define ENC_A_PIN                      GPIO_NUM_15    /* 编码器 A 相 */
#define ENC_B_PIN                      GPIO_NUM_17    /* 编码器 B 相 */
#define ENC_E_PIN                      GPIO_NUM_9     /* 编码器按键（E） */
#define ENCODER_POLL_INTERVAL_MS       2
#define ENCODER_BUTTON_DEBOUNCE_MS     30
#define ENCODER_COUNTS_PER_DETENT      4              /* 4倍解码：每个物理档位 4 次有效跳变 */
#define ENCODER_DUTY_STEP              50             /* 每个档位调整的占空比步进 */
#define ENCODER_BRIGHTNESS_STEP        33              /* 每个档位调整的亮度步进：30格（1.5圈）完成10-1000的渐变行程（990/30=33），确保快转时1.5圈完成全量程 */
#define ENCODER_FAST_ROTATION_THRESHOLD_MS 100        /* 快速旋转检测阈值：100ms内 */
#define ENCODER_FAST_ROTATION_DETENTS  3               /* 快速旋转阈值：100ms内旋转3格以上 */
#define ENCODER_CONTINUOUS_ROTATION_WINDOW_MS 500      /* 连续旋转检测窗口：500ms内连续旋转视为快速旋转 */
#define ENCODER_CONTINUOUS_ROTATION_COUNT 3            /* 连续旋转阈值：500ms内旋转3次以上视为快速旋转 */
#define ENCODER_FAST_ROTATION_FADE_MS_MIN  100         /* 快速旋转最小渐变时长：100ms确保快转时立即响应，避免渐变导致需要更多圈数 */
#define ENCODER_FAST_ROTATION_FADE_MS_MAX  1500        /* 快速旋转最大渐变时长：1500ms（30格约1.5秒） */
#define ENCODER_FAST_ROTATION_FADE_MS_PER_DETENT  50   /* 每格渐变时长：50ms（30格×50ms=1500ms，1.5圈左右） */
#define ENCODER_SLOW_ROTATION_FADE_MS  1000           /* 慢速旋转渐变时长：1000ms */

/* 快速旋转渐变时长计算函数已移至 encoder.c */

/* 渐变参数 */
#define FADE_DURATION_MS               3000           /* 开/关渐变约 3s */
#define LONG_PRESS_FADE_DURATION_MS    5000           /* 长按渐变约 5s */
#define FADE_UPDATE_INTERVAL_MS        20
#define FADE_SIGMOID_STEEPNESS         10.0f          /* S 曲线陡峭度，越大前段越慢 */

/* 默认亮度（50%） */
#define DEFAULT_DUTY                   (PWM_DUTY_MAX / 2)
/* 最低亮度（0.5%） */
#define MIN_DUTY                       8             /* 1500 * 0.005 ≈ 8 */

/* 灯光模式 */
typedef enum {
    LIGHT_MODE_COLD = 0,
    LIGHT_MODE_WARM,
    LIGHT_MODE_MIXED,  /* 冷暖同时亮 */
    LIGHT_MODE_MAX
} LIGHT_MODE_E;

/* 离线模式启用标志 */
static volatile BOOL_T s_offline_mode_enabled = FALSE;

/* 电源状态 */
static volatile BOOL_T s_offline_power_on = FALSE;
static volatile LIGHT_MODE_E s_offline_light_mode = LIGHT_MODE_COLD;

/* 当前PWM占空比 */
volatile UINT16_T s_offline_current_duty_cold = 0;
volatile UINT16_T s_offline_current_duty_warm = 0;

/* 上次关灯前的占空比（用于恢复亮度） */
static UINT16_T s_offline_last_duty_before_off = DEFAULT_DUTY;

/* 长按方向：TRUE=渐变亮，FALSE=渐变暗（默认先渐亮） */
BOOL_T s_offline_next_long_press_brighten = TRUE;

/* 渐变状态 */
volatile BOOL_T   s_offline_fade_active = FALSE;
static volatile UINT32_T s_offline_fade_start_ms = 0;
static volatile UINT16_T s_offline_fade_start_duty_cold = 0;
static volatile UINT16_T s_offline_fade_start_duty_warm = 0;
static volatile UINT16_T s_offline_fade_target_duty_cold = 0;
static volatile UINT16_T s_offline_fade_target_duty_warm = 0;
static volatile UINT32_T s_offline_fade_duration_ms = FADE_DURATION_MS;

/* 硬件控制优先级标志：硬件操作时，APP控制将被忽略 */
volatile BOOL_T s_hardware_control_active = FALSE;
volatile UINT32_T s_hardware_control_timeout_ms = 0;
#define HARDWARE_CONTROL_TIMEOUT_MS 500  /* 硬件操作后500ms内，APP控制被忽略 */

/* 编码器相关变量（需要被 encoder.c 访问） */
#if ENABLE_ENCODER_CONTROL
BOOL_T s_color_temp_reversed = FALSE;  /* 色温调整方向反转标志：到达边界时反转，松开按键后恢复 */
#endif

/* 渐变任务线程 */
static THREAD_HANDLE s_offline_fade_thread = NULL;

/* 断电计数清零定时器线程句柄 */
static THREAD_HANDLE s_power_cycle_reset_timer_thread = NULL;

/* 配网成功时间（系统启动后的时间戳，毫秒）- 仅保存在内存中，不保存到KV
 * 0表示未配网，非0表示配网成功的时间点
 * 注意：只有配网成功后（MQTT连接）才开始计时，确保时间准确 */
static UINT32_T s_power_cycle_provisioned_time = 0;

/* -------------------- PWM 控制函数 -------------------- */
/* 外部声明：运行模式状态（定义在tuya_app_main.c中） */
extern volatile RUN_MODE_E s_current_run_mode;

/* PWM占空比转换为在线模式的亮度和色温值 */
static void __duty_to_brightness_temp(UINT16_T duty_cold, UINT16_T duty_warm, 
                                      UINT16_T *brightness, UINT16_T *color_temp)
{
    UINT32_T total_duty = (UINT32_T)duty_cold + (UINT32_T)duty_warm;
    
    if (total_duty == 0) {
        *brightness = 0;
        *color_temp = 500;  /* 默认中间色温 */
        return;
    }
    
    /* 计算亮度：总占空比 / PWM_DUTY_MAX * 1000 */
    *brightness = (UINT16_T)((total_duty * 1000 + PWM_DUTY_MAX / 2) / PWM_DUTY_MAX);
    if (*brightness > 1000) {
        *brightness = 1000;
    }
    
    /* 计算色温：冷光比例 * 1000 */
    /* 色温值：0=全暖，1000=全冷 */
    if (total_duty > 0) {
        UINT32_T cold_ratio = ((UINT32_T)duty_cold * 1000) / total_duty;
        *color_temp = (UINT16_T)cold_ratio;
    } else {
        *color_temp = 500;  /* 默认中间色温 */
    }
}

/* 在线模式的亮度和色温值转换为PWM占空比 */
static void __brightness_temp_to_duty(UINT16_T brightness, UINT16_T color_temp,
                                      UINT16_T *duty_cold, UINT16_T *duty_warm)
{
    if (brightness == 0) {
        *duty_cold = 0;
        *duty_warm = 0;
        return;
    }
    
    /* 限制范围 */
    if (brightness > 1000) brightness = 1000;
    if (color_temp > 1000) color_temp = 1000;
    
    /* 计算占空比 */
    UINT32_T cold_ratio = color_temp;
    UINT32_T warm_ratio = 1000 - color_temp;
    
    UINT32_T cold = (UINT32_T)brightness * cold_ratio * PWM_DUTY_MAX;
    UINT32_T warm = (UINT32_T)brightness * warm_ratio * PWM_DUTY_MAX;
    
    *duty_cold = (UINT16_T)((cold + 500000) / 1000000);
    *duty_warm = (UINT16_T)((warm + 500000) / 1000000);
    
    if (*duty_cold > PWM_DUTY_MAX) *duty_cold = PWM_DUTY_MAX;
    if (*duty_warm > PWM_DUTY_MAX) *duty_warm = PWM_DUTY_MAX;
}

static void __offline_apply_pwm_direct(UINT16_T duty_cold, UINT16_T duty_warm)
{
    /* 移除模式检查，任何模式下都执行PWM控制 */
    (void)tkl_pwm_duty_set(PWM_COLD_CHANNEL, duty_cold);
    (void)tkl_pwm_duty_set(PWM_WARM_CHANNEL, duty_warm);
    (void)tkl_pwm_start(PWM_COLD_CHANNEL);
    (void)tkl_pwm_start(PWM_WARM_CHANNEL);

    /* 同步更新两个状态变量，确保状态一致 */
    s_offline_current_duty_cold = duty_cold;
    s_offline_current_duty_warm = duty_warm;
    /* 同步更新在线模式的状态变量，确保状态一致 */
    s_current_duty_cold = duty_cold;
    s_current_duty_warm = duty_warm;
}

/* -------------------- 硬件控制优先级函数 -------------------- */
VOID __set_hardware_control_active(VOID)
{
    UINT32_T now = tal_system_get_millisecond();
    s_hardware_control_active = TRUE;
    s_hardware_control_timeout_ms = now + HARDWARE_CONTROL_TIMEOUT_MS;
    TAL_PR_DEBUG("[HARDWARE_CTRL] Hardware control activated, timeout at %lu", s_hardware_control_timeout_ms);
}

/* -------------------- 渐变相关函数 -------------------- */
static inline void __offline_stop_fade(void)
{
    s_offline_fade_active = FALSE;
    /* 同时停止在线模式的渐变，避免冲突 */
    s_fade_active = FALSE;
}

static float __smooth_sigmoid_ease(float progress)
{
    if (progress <= 0.0f) {
        return 0.0f;
    }
    if (progress >= 1.0f) {
        return 1.0f;
    }

    const float steepness = FADE_SIGMOID_STEEPNESS;
    const float center = 0.55f;                    /* 稍晚一些再加速 */
    float steep = steepness;
    if (steep < 1.0f) {
        steep = 1.0f;
    }

    /* 标准逻辑函数：L(t) = 1 / (1 + e^{-k(t-c)}) */
    float logistic = 1.0f / (1.0f + expf(-steep * (progress - center)));
    float logistic_min = 1.0f / (1.0f + expf(-steep * (0.0f - center)));
    float logistic_max = 1.0f / (1.0f + expf(-steep * (1.0f - center)));

    float normalized = (logistic - logistic_min) / (logistic_max - logistic_min);

    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    /* 再套一层 smootherstep，让导数在两端均为 0，避免突兀感 */
    float smooth = normalized * normalized * normalized * (normalized * (normalized * 6.0f - 15.0f) + 10.0f);
    if (smooth < 0.0f) smooth = 0.0f;
    if (smooth > 1.0f) smooth = 1.0f;

    /* 压暗前段：gamma 校正，与在线模式一致 */
    const float gamma = 2.2f;
    float adjusted = powf(smooth, gamma);
    if (adjusted < 0.0f) adjusted = 0.0f;
    if (adjusted > 1.0f) adjusted = 1.0f;

    return adjusted;
}

static void __offline_start_fade(UINT16_T target_cold, UINT16_T target_warm, UINT32_T duration_ms)
{
    UINT32_T fade_duration = (duration_ms == 0) ? FADE_DURATION_MS : duration_ms;
    
    /* 停止在线模式的渐变，避免冲突 */
    s_fade_active = FALSE;
    
    /* 如果正在渐变，智能处理目标值更新 */
    if (s_offline_fade_active) {
        /* 计算当前目标值与新目标值的差异 */
        INT32_T cold_diff = (INT32_T)target_cold - (INT32_T)s_offline_fade_target_duty_cold;
        INT32_T warm_diff = (INT32_T)target_warm - (INT32_T)s_offline_fade_target_duty_warm;
        UINT32_T cold_diff_abs = (cold_diff < 0) ? (UINT32_T)(-cold_diff) : (UINT32_T)cold_diff;
        UINT32_T warm_diff_abs = (warm_diff < 0) ? (UINT32_T)(-warm_diff) : (UINT32_T)warm_diff;
        UINT32_T total_diff = cold_diff_abs + warm_diff_abs;
        
        /* 优化策略：对于编码器连续调整，动态更新起始点为当前PWM输出值
         * 这样可以避免频繁重启导致的灯光抖动，实现平滑的连续调整
         * 渐变会从当前位置平滑过渡到新目标值 */
        if (total_diff > 0) {
            /* 更新起始点为当前PWM输出值（渐变任务实时更新的值）
             * 这样渐变会从当前位置平滑过渡到新目标值
             * 重置时间，但保持较短的渐变时间，实现快速响应 */
            if (s_current_duty_cold > 0 || s_current_duty_warm > 0) {
                s_offline_fade_start_duty_cold = s_current_duty_cold;
                s_offline_fade_start_duty_warm = s_current_duty_warm;
            } else {
                s_offline_fade_start_duty_cold = s_offline_current_duty_cold;
                s_offline_fade_start_duty_warm = s_offline_current_duty_warm;
            }
            /* 重置时间，但使用较短的渐变时间，实现快速响应 */
            s_offline_fade_start_ms = tal_system_get_millisecond();
            s_offline_fade_duration_ms = fade_duration;
        } else {
            /* 目标值没有变化，不需要更新 */
            return;
        }
    } else {
        /* 未在渐变，正常启动 */
        /* 优先使用在线模式的状态变量（如果在线模式有值），否则使用离线模式的状态变量 */
        if (s_current_duty_cold > 0 || s_current_duty_warm > 0) {
            s_offline_fade_start_duty_cold = s_current_duty_cold;
            s_offline_fade_start_duty_warm = s_current_duty_warm;
        } else {
            s_offline_fade_start_duty_cold = s_offline_current_duty_cold;
            s_offline_fade_start_duty_warm = s_offline_current_duty_warm;
        }
        s_offline_fade_start_ms = tal_system_get_millisecond();
        s_offline_fade_duration_ms = fade_duration;
    }
    
    /* 更新目标值 */
    s_offline_fade_target_duty_cold = target_cold;
    s_offline_fade_target_duty_warm = target_warm;
    s_offline_fade_active = TRUE;
}

static void __offline_compute_target_duty(UINT16_T *duty_cold, UINT16_T *duty_warm)
{
    if (!s_offline_power_on) {
        *duty_cold = 0;
        *duty_warm = 0;
        return;
    }

    UINT16_T target_duty = s_offline_last_duty_before_off;
    
    switch (s_offline_light_mode) {
        case LIGHT_MODE_COLD:
            *duty_cold = target_duty;
            *duty_warm = 0;
            break;
        case LIGHT_MODE_WARM:
            *duty_cold = 0;
            *duty_warm = target_duty;
            break;
        case LIGHT_MODE_MIXED:
            /* 冷暖同时亮：各占一半亮度 */
            *duty_cold = target_duty / 2;
            *duty_warm = target_duty / 2;
            break;
        default:
            *duty_cold = 0;
            *duty_warm = 0;
            break;
    }
}

static void __offline_apply_state_with_fade(void)
{
    /* 注意：模式切换只能通过 set_run_mode() 手动设置，不会自动切换 */
    /* 只有在 RUN_MODE_OFFLINE 模式下才会执行PWM控制 */
    
    UINT16_T target_cold = 0;
    UINT16_T target_warm = 0;

    if (s_offline_power_on) {
        /* 开灯：根据灯光模式计算占空比 */
        __offline_compute_target_duty(&target_cold, &target_warm);
    } else {
        /* 关灯：保存当前占空比，然后渐变到0 */
        if (s_offline_current_duty_cold > 0) {
            s_offline_last_duty_before_off = s_offline_current_duty_cold;
        } else if (s_offline_current_duty_warm > 0) {
            s_offline_last_duty_before_off = s_offline_current_duty_warm;
        }
        target_cold = 0;
        target_warm = 0;
    }

    if (target_cold == s_offline_current_duty_cold && target_warm == s_offline_current_duty_warm) {
        __offline_stop_fade();
        return;
    }

    __offline_start_fade(target_cold, target_warm, FADE_DURATION_MS);
}

VOID __offline_cycle_light_mode(VOID)
{
    /* 如果正在渐变，不接受切换命令 */
    if (s_offline_fade_active) {
        return;
    }

    /* 设置硬件控制优先级标志 */
    /* 编码器旋转固定 1s 渐变：通过 encoder flag 在 tuya_app_main.c 处理 */
    /* 注意：模式切换只能通过 set_run_mode() 手动设置，不会自动切换 */
    /* 只有在 RUN_MODE_OFFLINE 模式下才会执行PWM控制 */

    s_offline_light_mode = (LIGHT_MODE_E)((s_offline_light_mode + 1) % LIGHT_MODE_MAX);
    s_offline_power_on = TRUE;
    
    /* 如果当前有输出，切换模式时保持亮度 */
    if (s_offline_current_duty_cold > 0 || s_offline_current_duty_warm > 0) {
        s_offline_last_duty_before_off = (s_offline_current_duty_cold > 0) ? s_offline_current_duty_cold : s_offline_current_duty_warm;
    }
    
    __offline_apply_state_with_fade();
    const CHAR_T *mode_str = "COLD";
    if (s_offline_light_mode == LIGHT_MODE_WARM) {
        mode_str = "WARM";
    } else if (s_offline_light_mode == LIGHT_MODE_MIXED) {
        mode_str = "MIXED";
    }
    TAL_PR_NOTICE("Light mode switched to: %s", mode_str);
}

VOID __offline_adjust_brightness(INT8_T step_delta)
{
    if (step_delta == 0) {
        return;
    }

    /* 编码器旋转固定 1s 渐变：通过 encoder flag 在 tuya_app_main.c 处理 */

    /* 优先使用实际PWM值计算当前亮度，确保渐变过程中计算正确 */
    UINT32_T total_duty = (UINT32_T)s_current_duty_cold + (UINT32_T)s_current_duty_warm;
    UINT16_T current_brightness = 0;
    if (total_duty > 0) {
        /* 从实际PWM值计算当前亮度 */
        current_brightness = (UINT16_T)((total_duty * 1000 + PWM_DUTY_MAX / 2) / PWM_DUTY_MAX);
        if (current_brightness > 1000) {
            current_brightness = 1000;
        }
    }
    
    /* 如果实际亮度为0（灯关闭），使用存储值 */
    UINT16_T stored_brightness = light_control_get_brightness();
    if (current_brightness == 0 && stored_brightness > 0) {
        current_brightness = stored_brightness;
    }
    
    UINT16_T current_color_temp = light_control_get_color_temp();

    /* 灯关且继续减亮度忽略；亮度已到最暗(10)再左旋也忽略 */
    if ((current_brightness == 0 && step_delta < 0) ||
        (current_brightness <= 10 && step_delta < 0)) {
        TAL_PR_DEBUG("Brightness: at min or OFF and rotating left, no action");
        return;
    }

    INT32_T target_brightness = (INT32_T)current_brightness + (INT32_T)step_delta * 100;  /* 每档100 */
    if (current_brightness == 0 && step_delta > 0) {
        /* 灯关时从0起步，首档按步进计算（后续最小值再夹） */
        target_brightness = (INT32_T)step_delta * 100;
    }
    if (target_brightness < 10) {
        target_brightness = 10;
        /* 已在最暗且继续减，不动作 */
        if (step_delta < 0) {
            return;
        }
    }
    if (target_brightness > 1000) target_brightness = 1000;

    UINT16_T duty_cold_log = 0;
    UINT16_T duty_warm_log = 0;
    __brightness_temp_to_duty((UINT16_T)target_brightness, current_color_temp, &duty_cold_log, &duty_warm_log);

    /* 统一走在线模式接口，标记编码器来源以固定 1s 渐变 */
    light_control_set_encoder_dim_flag(TRUE);
    light_control_set_brightness((UINT16_T)target_brightness);
    light_control_set_encoder_dim_flag(FALSE);

    if (target_brightness > 0 && !light_control_get()) {
        light_control_set(TRUE);
    }

    /* 编码器调整灯光时不上传到云端，减少网络开销 */
    // upload_device_all_status();
}

/* 前向声明 */
VOID __offline_adjust_color_temp_with_base(INT8_T step_delta, UINT16_T base_color_temp);

VOID __offline_adjust_color_temp(INT8_T step_delta)
{
    __offline_adjust_color_temp_with_base(step_delta, 0);  /* 0表示使用当前实际值 */
}

VOID __offline_adjust_color_temp_with_base(INT8_T step_delta, UINT16_T base_color_temp)
{
    if (step_delta == 0) {
        return;
    }

    /* 编码器旋转固定 1s 渐变：通过 encoder flag 在 tuya_app_main.c 处理 */

    UINT16_T current_brightness = light_control_get_brightness();
    /* 如果提供了base_color_temp，使用它作为当前值（用于快速旋转时的边界检测） */
    UINT16_T current_color_temp = (base_color_temp > 0) ? base_color_temp : light_control_get_color_temp();

    if (current_brightness == 0) {
        TAL_PR_DEBUG("Color temp: brightness is 0, cannot adjust color temp");
        return;
    }

    /* 循环色温调整：到达边界时反转旋转方向，松开按键后恢复 */
    INT8_T effective_step = step_delta;
    
    /* 如果处于反转状态，先反转方向 */
    if (s_color_temp_reversed) {
        effective_step = -step_delta;
    }
    
    /* 计算目标色温值 */
    INT32_T target_color_temp = (INT32_T)current_color_temp + (INT32_T)effective_step * 100;  /* 每档100 */
    
    /* 检查是否到达边界，如果到达边界则设置反转状态并反转方向 */
    if (target_color_temp < 0) {
        /* 在最暖(0)时左旋，设置反转状态并反转成右旋（开始变冷） */
        s_color_temp_reversed = TRUE;
        target_color_temp = -target_color_temp;  /* 反转：从0开始向右（变冷） */
    } else if (target_color_temp > 1000) {
        /* 在最冷(1000)时右旋，设置反转状态并反转成左旋（开始变暖） */
        s_color_temp_reversed = TRUE;
        target_color_temp = 2000 - target_color_temp;  /* 反转：从1000开始向左（变暖） */
    }
    
    /* 限制在有效范围内 */
    if (target_color_temp < 0) target_color_temp = 0;
    if (target_color_temp > 1000) target_color_temp = 1000;

    UINT16_T duty_cold_log = 0;
    UINT16_T duty_warm_log = 0;
    __brightness_temp_to_duty(current_brightness, (UINT16_T)target_color_temp, &duty_cold_log, &duty_warm_log);

    light_control_set_encoder_dim_flag(TRUE);
    light_control_set_color_temp((UINT16_T)target_color_temp);
    light_control_set_encoder_dim_flag(FALSE);

    /* 编码器调整灯光时不上传到云端，减少网络开销 */
    // upload_device_all_status();
}

static void __offline_toggle_power_with_fade(void)
{
    /* 设置硬件控制优先级标志 */
    __set_hardware_control_active();
    
    s_offline_power_on = !s_offline_power_on;
    __offline_apply_state_with_fade();
    TAL_PR_NOTICE("Power %s", s_offline_power_on ? "ON" : "OFF");
}

static void __offline_fade_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 移除模式检查，任何模式下都可用 */
        /* 如果在线模式的渐变正在运行，停止离线模式渐变，避免冲突 */
        if (s_fade_active) {
            s_offline_fade_active = FALSE;
            /* 实时同步状态变量：从在线模式的状态变量读取（在线模式渐变任务会更新这些值） */
            s_offline_current_duty_cold = s_current_duty_cold;
            s_offline_current_duty_warm = s_current_duty_warm;
        } else if (s_offline_fade_active) {
            UINT32_T now = tal_system_get_millisecond();
            UINT32_T elapsed = now - s_offline_fade_start_ms;

            if (elapsed >= s_offline_fade_duration_ms) {
                __offline_apply_pwm_direct(s_offline_fade_target_duty_cold, s_offline_fade_target_duty_warm);
                /* 同步更新在线模式的状态变量 */
                s_current_duty_cold = s_offline_fade_target_duty_cold;
                s_current_duty_warm = s_offline_fade_target_duty_warm;
                s_offline_fade_active = FALSE;
            } else {
                float progress = (float)elapsed / (float)s_offline_fade_duration_ms;
                float eased = __smooth_sigmoid_ease(progress);

                float duty_cold = (float)s_offline_fade_start_duty_cold +
                                  ((float)s_offline_fade_target_duty_cold - (float)s_offline_fade_start_duty_cold) * eased;
                float duty_warm = (float)s_offline_fade_start_duty_warm +
                                  ((float)s_offline_fade_target_duty_warm - (float)s_offline_fade_start_duty_warm) * eased;

                UINT16_T current_duty_cold = (UINT16_T)(duty_cold + 0.5f);
                UINT16_T current_duty_warm = (UINT16_T)(duty_warm + 0.5f);
                
                __offline_apply_pwm_direct(current_duty_cold, current_duty_warm);
                /* 同步更新在线模式的状态变量 */
                s_current_duty_cold = current_duty_cold;
                s_current_duty_warm = current_duty_warm;
            }
        }

        tal_system_sleep(FADE_UPDATE_INTERVAL_MS);
    }
}

/* -------------------- 编码器相关函数（已移至 encoder.c） -------------------- */
#if ENABLE_ENCODER_CONTROL
/* 编码器相关函数已移至 encoder.c 模块 */
#endif

/* -------------------- PWM 初始化 -------------------- */
static void __offline_pwm_init(void)
{
    TUYA_PWM_BASE_CFG_T cfg_cold = {
        .polarity  = TUYA_PWM_POSITIVE,
        .duty      = 0,
        .frequency = PWM_FREQUENCY_HZ,
    };

    TUYA_PWM_BASE_CFG_T cfg_warm = {
        .polarity  = TUYA_PWM_POSITIVE,
        .duty      = 0,
        .frequency = PWM_FREQUENCY_HZ,
    };

    (void)tkl_pwm_init(PWM_COLD_CHANNEL, &cfg_cold);
    (void)tkl_pwm_start(PWM_COLD_CHANNEL);
    (void)tkl_pwm_init(PWM_WARM_CHANNEL, &cfg_warm);
    (void)tkl_pwm_start(PWM_WARM_CHANNEL);

    s_offline_current_duty_cold = 0;
    s_offline_current_duty_warm = 0;

    TAL_PR_NOTICE("PWM initialized: Cold=P7, Warm=P8, Freq=%d Hz, Max Duty=%d", 
                  PWM_FREQUENCY_HZ, PWM_DUTY_MAX);
}

/* -------------------- 断电复位检测 -------------------- */
/* 断电计数数据结构（保存到KV存储） */
typedef struct {
    UINT32_T count;          /* 断电计数（连续断电累计） */
} POWER_CYCLE_DATA_T;

/**
 * @brief 断电复位检查函数（在每次上电时调用）
 * 功能：每次上电时计数+1，如果累计4次连续断电（配网成功后运行时间<=1分钟），则重置配网
 * 注意：1分钟计时从配网成功后（MQTT连接）开始，确保时间准确
 */
VOID __offline_check_power_cycle_reset(VOID)
{
    POWER_CYCLE_DATA_T cycle_data = {0};
    UINT32_T old_count = 0;
    
    /* 初始化配网时间（等待配网成功后才会记录） */
    s_power_cycle_provisioned_time = 0;
    
    /* 从KV读取保存的断电计数 */
    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET ret = wd_common_read(KV_KEY_POWER_CYCLE_COUNT, &buf, &len);
    
    if (ret == OPRT_OK && buf != NULL && len == sizeof(cycle_data)) {
        memcpy(&cycle_data, buf, sizeof(cycle_data));
        old_count = cycle_data.count;
        wd_common_free_data(buf);
        buf = NULL;
    } else {
        /* KV中没有数据，初始化为0 */
        memset(&cycle_data, 0, sizeof(cycle_data));
    }
    
    /* 每次上电时，计数+1 */
    old_count = cycle_data.count;
        cycle_data.count++;
    TAL_PR_NOTICE("[POWER_CYCLE] 断电计数: %d -> %d", old_count, cycle_data.count);
    
    /* 保存到KV存储 */
    ret = wd_common_write(KV_KEY_POWER_CYCLE_COUNT, (BYTE_T *)&cycle_data, sizeof(cycle_data));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[POWER_CYCLE] KV写入失败: ret=%d", ret);
    }
    
    /* 如果达到复位次数，执行复位 */
    if (cycle_data.count >= POWER_CYCLE_RESET_COUNT) {
        TAL_PR_NOTICE("[POWER_CYCLE] 触发断电复位 (count=%d >= %d)", 
                     cycle_data.count, POWER_CYCLE_RESET_COUNT);
        
        /* 清零KV计数 */
        memset(&cycle_data, 0, sizeof(cycle_data));
        ret = wd_common_write(KV_KEY_POWER_CYCLE_COUNT, (BYTE_T *)&cycle_data, sizeof(cycle_data));
        if (ret != OPRT_OK) {
            TAL_PR_ERR("[POWER_CYCLE] 清零计数失败: ret=%d", ret);
        }
        
        /* 检查网络状态，如果网络服务未初始化，等待一段时间 */
        NETWORK_STATUS_E net_status = tuya_svc_netmgr_get_status();
        if (net_status == NETWORK_STATUS_OFFLINE || net_status == 0) {
            tal_system_sleep(3000);
            net_status = tuya_svc_netmgr_get_status();
        }
        
        /* 解绑设备 */
        ret = tuya_iot_wf_gw_unactive();
        if (ret == OPRT_OK) {
            TAL_PR_NOTICE("[POWER_CYCLE] 设备解绑成功，进入配网模式");
        } else {
            TAL_PR_ERR("[POWER_CYCLE] 设备解绑失败: ret=%d (KV已清除)", ret);
        }
    }
}

/**
 * @brief 通知配网成功，开始计时
 * 功能：在MQTT连接成功时调用，记录配网成功时间，开始1分钟计时
 */
VOID __offline_notify_provisioned(VOID)
{
    if (s_power_cycle_provisioned_time == 0) {
        s_power_cycle_provisioned_time = tal_system_get_millisecond();
        TAL_PR_NOTICE("[POWER_CYCLE] 配网成功，开始计时");
    }
}

/**
 * @brief 断电计数运行时间检查定时器线程
 * 功能：定期检查配网成功后的运行时间，如果超过1分钟则清零KV计数
 * 注意：只有配网成功后（MQTT连接）才开始计时，确保时间准确
 */
STATIC VOID_T __power_cycle_timer_task(VOID_T *arg)
{
    (VOID)arg;
    
    while (1) {
        tal_system_sleep(POWER_CYCLE_CHECK_INTERVAL_MS);
        
        UINT32_T now = tal_system_get_millisecond();
        POWER_CYCLE_DATA_T cycle_data = {0};
        
        /* 如果配网时间为0，说明还未配网成功，跳过检查 */
        if (s_power_cycle_provisioned_time == 0) {
            /* 检查是否已配网成功（MQTT连接） */
            NETWORK_STATUS_E net_status = tuya_svc_netmgr_get_status();
            if (net_status == NETWORK_STATUS_MQTT) {
                /* MQTT已连接，但还未记录配网时间，现在记录 */
                s_power_cycle_provisioned_time = now;
            } else {
                /* 未配网，继续等待 */
                continue;
            }
        }
        
        /* 从KV读取保存的断电计数 */
        BYTE_T *buf = NULL;
        UINT_T len = 0;
        OPERATE_RET ret = wd_common_read(KV_KEY_POWER_CYCLE_COUNT, &buf, &len);
        
        if (ret == OPRT_OK && buf != NULL && len == sizeof(cycle_data)) {
            memcpy(&cycle_data, buf, sizeof(cycle_data));
            wd_common_free_data(buf);
            buf = NULL;
            
            /* 如果计数为0，说明已经复位或未开始，跳过检查 */
            if (cycle_data.count == 0) {
                continue;
            }
            
            /* 检查运行时间是否超过1分钟（从配网成功时间开始计算） */
            UINT32_T run_time = 0;
            if (now >= s_power_cycle_provisioned_time) {
                run_time = now - s_power_cycle_provisioned_time;
            } else {
                /* 时间戳回绕，说明已经运行了很长时间，肯定超过1分钟 */
                run_time = POWER_CYCLE_WINDOW_MS + 1;
            }
            
            if (run_time > POWER_CYCLE_WINDOW_MS) {
                /* 配网成功后运行时间超过1分钟，清零计数 */
                UINT32_T old_count = cycle_data.count;
                memset(&cycle_data, 0, sizeof(cycle_data));
                
                TAL_PR_NOTICE("[POWER_CYCLE_TIMER] 运行时间超过1分钟，清零计数: %d -> 0", old_count);
                
                ret = wd_common_write(KV_KEY_POWER_CYCLE_COUNT, (BYTE_T *)&cycle_data, sizeof(cycle_data));
                if (ret != OPRT_OK) {
                    TAL_PR_ERR("[POWER_CYCLE_TIMER] 清零计数失败: ret=%d", ret);
                }
            }
        }
    }
}

/* -------------------- 离线模式初始化 -------------------- */
static void __offline_app_init(void)
{
    /* 注意：编码器和触摸板控制应该在任何时候都可用，不仅仅是在离线模式下
     * 因此，这里不检查 s_offline_mode_enabled，确保硬件控制线程始终启动
     * 这样即使在配网过程中或在线模式下，用户也能使用硬件控制
     */
    
#if ENABLE_TOUCH_CONTROL
    touch_init();
    touch_start_task();
#endif

#if ENABLE_ENCODER_CONTROL
    encoder_init();
    encoder_start_task();
#endif
    
    TAL_PR_NOTICE("[OFFLINE] Input control ready - Encoder/Touch using online logic");
}

/* -------------------- 公共接口函数 -------------------- */
VOID offline_mode_init(VOID)
{
    /* 注意：断电复位检查已在 user_main() 中调用，这里不再重复调用 */
    
    /* 启动断电计数窗口过期检查定时器线程 */
    if (s_power_cycle_reset_timer_thread == NULL) {
        THREAD_CFG_T timer_cfg = {
            .thrdname   = "power_cycle_timer",
            .priority   = THREAD_PRIO_6,
            .stackDepth = 2048,
        };
        OPERATE_RET ret = tal_thread_create_and_start(&s_power_cycle_reset_timer_thread, NULL, NULL,
                                                       __power_cycle_timer_task, NULL, &timer_cfg);
        if (ret != OPRT_OK) {
            TAL_PR_ERR("[POWER_CYCLE] 定时器线程启动失败: ret=%d", ret);
        }
    }
    
    /* 上电后不默认启用离线模式，正常走配网流程（配网成功则进入在线，否则进入离线） */
    s_offline_mode_enabled = FALSE;
    
    /* 注意：运行模式需要通过 set_run_mode() 手动设置 */
    /* 默认模式在 tuya_app_main.c 中初始化时设置 */
    /* 如需修改默认模式，请修改 tuya_app_main.c 中的 s_current_run_mode 初始值 */
    
    /* 初始化硬件控制线程（即使离线模式未启用，也先初始化，以便配网超时后能立即使用） */
    __offline_app_init();
}

VOID offline_mode_enable(BOOL_T enable)
{
    s_offline_mode_enabled = enable;
    if (enable) {
        TAL_PR_NOTICE("[OFFLINE] Offline mode enabled");
        /* 确保触摸板和编码器线程已启动 */
        __offline_app_init();
        
        /* 停止配网服务，确保设备在离线模式下不可被手机搜索到 */
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
        /* 1. 停止高层配网服务（包括SmartConfig等） */
        extern OPERATE_RET tuya_wifi_netcfg_stop(VOID);
        OPERATE_RET ret = tuya_wifi_netcfg_stop();
        if (ret == OPRT_OK) {
            TAL_PR_NOTICE("[OFFLINE] Provisioning service stopped successfully");
        } else {
            TAL_PR_ERR("[OFFLINE] Failed to stop provisioning service, ret=%d, will retry", ret);
            /* 如果停止失败，延迟后重试 */
            tal_system_sleep(100);
            tuya_wifi_netcfg_stop();
            TAL_PR_NOTICE("[OFFLINE] Provisioning service stop retried");
        }
        
        /* 2. 直接停止WiFi AP模式（热点），确保设备热点被关闭，参考：https://developer.tuya.com/cn/docs/iot-device-dev/TuyaOS-iot_abi_driver_wifi?id=Kcusut0tv85ee */
        OPERATE_RET ap_ret = tkl_wifi_stop_ap();
        if (ap_ret == OPRT_OK) {
            TAL_PR_NOTICE("[OFFLINE] WiFi AP mode stopped successfully, device hotspot closed");
        } else {
            TAL_PR_ERR("[OFFLINE] Failed to stop WiFi AP mode, ret=%d (may not be in AP mode)", ap_ret);
        }
        
        /* 3. 停止BLE广播，确保设备在离线模式下不可通过BLE被搜索到 */
        OPERATE_RET ble_ret = tal_ble_advertising_stop();
        if (ble_ret == OPRT_OK) {
            TAL_PR_NOTICE("[OFFLINE] BLE advertising stopped successfully, device not discoverable via BLE");
        } else {
            TAL_PR_ERR("[OFFLINE] Failed to stop BLE advertising, ret=%d (may not be advertising)", ble_ret);
        }
        
        TAL_PR_NOTICE("[OFFLINE_STATUS] ===== 离线模式下不可被手机搜索到 =====");
        TAL_PR_NOTICE("[OFFLINE_STATUS] Offline mode enabled - Provisioning service: STOPPED, WiFi AP: STOPPED, BLE: STOPPED, Device discoverable: NO");
#endif
    } else {
        TAL_PR_NOTICE("[OFFLINE] Offline mode disabled");
    }
}

BOOL_T offline_mode_is_enabled(VOID)
{
    return s_offline_mode_enabled;
}

/* 模式状态控制 */
VOID set_run_mode(RUN_MODE_E mode)
{
    s_current_run_mode = mode;
    const CHAR_T *mode_str = (mode == RUN_MODE_ONLINE) ? "ONLINE" : "OFFLINE";
    TAL_PR_NOTICE("[MODE] Run mode set to: %s", mode_str);
}

RUN_MODE_E get_run_mode(VOID)
{
    return s_current_run_mode;
}
