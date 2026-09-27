/**
 * @file touch.c
 * @brief 触摸板控制模块实现
 */

#include <string.h>
#include "tuya_cloud_types.h"
#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "tkl_gpio.h"
#include "touch.h"

/* 外部函数声明 */
extern BOOL_T light_control_network_indicator_active(VOID);
extern VOID light_control_set(BOOL_T on);
extern VOID light_control_set_color_temp(UINT16_T value);
extern VOID light_control_set_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);
extern BOOL_T light_control_get(VOID);
extern UINT16_T light_control_get_color_temp(VOID);
extern VOID upload_device_all_status(VOID);
extern VOID __set_hardware_control_active(VOID);
extern BOOL_T s_offline_next_long_press_brighten;

/* 触摸板配置 */
#define TOUCH_PIN                      GPIO_NUM_22
#define TOUCH_DEBOUNCE_MS              30
#define TOUCH_DOUBLE_CLICK_MS          350
#define TOUCH_POLL_INTERVAL_MS         10
#define TOUCH_LONG_PRESS_THRESHOLD_MS  600
#define TOUCH_SECOND_HOLD_MS           2000
#define TOUCH_RESET_THRESHOLD_MS       10000

static THREAD_HANDLE s_touch_thread = NULL;
static TUYA_GPIO_LEVEL_E s_touch_idle_level = TUYA_GPIO_LEVEL_HIGH;
static BOOL_T s_touch_idle_initialized = FALSE;

/* 触摸板状态检测 */
static BOOL_T touch_pressed(VOID)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    (void)tkl_gpio_read(TOUCH_PIN, &level);

    if (!s_touch_idle_initialized) {
        s_touch_idle_level = level;
        s_touch_idle_initialized = TRUE;
    }

    return (level != s_touch_idle_level) ? TRUE : FALSE;
}

/* GPIO初始化 */
static VOID touch_gpio_init(VOID)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode   = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };

    (void)tkl_gpio_init(TOUCH_PIN, &cfg);
    TAL_PR_NOTICE("Touch GPIO initialized: GPIO%d", TOUCH_PIN);
}

/* 触摸板主任务 */
static VOID touch_task(VOID *arg)
{
    (void)arg;

    UINT32_T now = tal_system_get_millisecond();
    BOOL_T stable_pressed = touch_pressed();
    UINT32_T stable_change_ms = now;
    UINT32_T press_start_ms = stable_pressed ? now : 0;

    BOOL_T double_click_pending = FALSE;
    UINT32_T double_click_deadline_ms = 0;
    UINT32_T first_click_time = 0;
    BOOL_T waiting_for_double_click = FALSE;
    BOOL_T second_press_active = FALSE;
    UINT32_T second_press_start_ms = 0;
    BOOL_T double_click_confirmed = FALSE;
    BOOL_T long_press_active = FALSE;

    TAL_PR_NOTICE("[TOUCH] Touch task started");

    for (;;) {
        now = tal_system_get_millisecond();
        BOOL_T raw_pressed = touch_pressed();

        /* 更新触摸状态 */
        if (raw_pressed != stable_pressed) {
            if (now - stable_change_ms >= TOUCH_DEBOUNCE_MS) {
                stable_pressed = raw_pressed;
                stable_change_ms = now;

                if (stable_pressed) {
                    press_start_ms = now;
                    long_press_active = FALSE;
                    /* 检查是否为第二次点击 */
                    if (waiting_for_double_click &&
                        (now - first_click_time) <= TOUCH_DOUBLE_CLICK_MS) {
                        second_press_active = TRUE;
                        second_press_start_ms = now;
                        double_click_pending = FALSE;
                        waiting_for_double_click = FALSE;
                    } else {
                        second_press_active = FALSE;
                        double_click_confirmed = FALSE;
                    }
                } else {
                    /* 触摸释放 */
                    if (double_click_confirmed) {
                        second_press_active = FALSE;
                        double_click_pending = FALSE;
                        waiting_for_double_click = FALSE;
                        long_press_active = FALSE;
                        continue;
                    }

                    if (second_press_active) {
                        UINT32_T held_ms = now - second_press_start_ms;
                        second_press_active = FALSE;
                        if (held_ms < TOUCH_SECOND_HOLD_MS) {
                            double_click_pending = FALSE;
                            waiting_for_double_click = FALSE;
                            continue;
                        }
                    }

                    if (long_press_active) {
                        long_press_active = FALSE;
                    } else {
                        waiting_for_double_click = TRUE;
                        first_click_time = now;
                        double_click_pending = TRUE;
                        double_click_deadline_ms = now + TOUCH_DOUBLE_CLICK_MS;
                    }
                }
            }
        }
        
        /* 复位检测 */
        if (stable_pressed) {
            UINT32_T held_ms = now - press_start_ms;
            if (held_ms >= TOUCH_RESET_THRESHOLD_MS) {
                /* 配网指示灯期间忽略复位操作 */
                if (light_control_network_indicator_active()) {
                    TAL_PR_NOTICE("[HARDWARE] 触摸板复位操作被忽略：配网灯光期间不允许硬件操作");
                    press_start_ms = now;
                } else {
                    TAL_PR_NOTICE("Touch reset detected (10s), removing device from cloud...");
                    extern OPERATE_RET tuya_iot_wf_gw_unactive(VOID);
                    OPERATE_RET ret = tuya_iot_wf_gw_unactive();
                    if (ret == OPRT_OK) {
                        TAL_PR_NOTICE("Device removed from cloud successfully, will enter provisioning mode");
                    } else {
                        TAL_PR_ERR("Failed to remove device from cloud, ret=%d", ret);
                    }
                    press_start_ms = now;
                }
            }
        }
        
        /* 功能处理 */
        if (stable_pressed) {
            /* 检测长按 */
            UINT32_T held_ms = now - press_start_ms;
            if (!long_press_active && held_ms >= TOUCH_LONG_PRESS_THRESHOLD_MS) {
                if (second_press_active) {
                    /* 等待双击2s逻辑处理 */
                } else {
                    if (light_control_network_indicator_active()) {
                        TAL_PR_NOTICE("[HARDWARE] 触摸板长按操作被忽略：配网灯光期间不允许硬件操作");
                        long_press_active = TRUE;
                        double_click_pending = FALSE;
                    } else {
                        long_press_active = TRUE;
                        double_click_pending = FALSE;
                        
                        UINT16_T target_brightness = s_offline_next_long_press_brighten ? 1000 : 10;
                        UINT16_T current_color_temp = light_control_get_color_temp();
                        TAL_PR_NOTICE("Long press detected, target brightness: %u", target_brightness);

                        s_offline_next_long_press_brighten = !s_offline_next_long_press_brighten;

                        light_control_set_state(target_brightness > 0, target_brightness, current_color_temp);
                        upload_device_all_status();
                    }
                }
            }

            /* 双击第二次按下时按住超过 2s：确认双击，执行色温切换 */
            if (second_press_active && !double_click_confirmed) {
                UINT32_T held_ms2 = now - second_press_start_ms;
                if (held_ms2 >= TOUCH_SECOND_HOLD_MS) {
                    if (light_control_network_indicator_active()) {
                        TAL_PR_NOTICE("[HARDWARE] 触摸板双击操作被忽略：配网灯光期间不允许硬件操作");
                    } else {
                        __set_hardware_control_active();
                        UINT16_T current_color_temp = light_control_get_color_temp();
                        UINT16_T next_color = 0;
                        if (current_color_temp > 666) {
                            next_color = 0;
                        } else if (current_color_temp < 333) {
                            next_color = 500;
                        } else {
                            next_color = 1000;
                        }
                        light_control_set_color_temp(next_color);
                        TAL_PR_NOTICE("Color temp switched (touch double click + hold 2s): %d -> %d",
                                      current_color_temp, next_color);
                        upload_device_all_status();
                    }
                    double_click_confirmed = TRUE;
                    double_click_pending = FALSE;
                    waiting_for_double_click = FALSE;
                }
            }
        }

        /* 单击处理 */
        if (double_click_pending && now > double_click_deadline_ms) {
            double_click_pending = FALSE;
            waiting_for_double_click = FALSE;
            
            if (light_control_network_indicator_active()) {
                TAL_PR_NOTICE("[HARDWARE] 触摸板单击操作被忽略：配网灯光期间不允许硬件操作");
                continue;
            }
            
            __set_hardware_control_active();
            BOOL_T current_state = light_control_get();
            light_control_set(!current_state);
            TAL_PR_NOTICE("Power %s (touch single click)", !current_state ? "ON" : "OFF");
            upload_device_all_status();
        }

        tal_system_sleep(TOUCH_POLL_INTERVAL_MS);
    }
}

/* 公共接口 */
VOID touch_init(VOID)
{
    touch_gpio_init();
}

VOID touch_start_task(VOID)
{
    if (s_touch_thread == NULL) {
        THREAD_CFG_T touch_cfg = {
            .thrdname   = "touch",
            .priority   = THREAD_PRIO_6,
            .stackDepth = 2048,
        };
        OPERATE_RET ret = tal_thread_create_and_start(&s_touch_thread, NULL, NULL, 
                                                       touch_task, NULL, &touch_cfg);
        if (ret == OPRT_OK) {
            TAL_PR_NOTICE("[TOUCH] Touch task started successfully");
        } else {
            TAL_PR_ERR("[TOUCH] Failed to start touch task, ret=%d", ret);
        }
    }
}

