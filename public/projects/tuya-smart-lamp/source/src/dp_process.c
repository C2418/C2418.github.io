/**
 * @file dp_process.c
 * @brief Minimal DP handling for App 1102
 */

#include "tuya_cloud_types.h"

#include "tuya_iot_com_api.h"
#include "tuya_ws_db.h"
#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "tal_time_service.h"
#include "tkl_rtc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern BOOL_T light_control_network_indicator_active(VOID);
VOID dp_dnd_state_delayed_save(VOID);  /* forward declaration to avoid implicit warning */

/***********************************************************/
/* **********************macro define************************ */
/***********************************************************/
#define DPID_SWITCH_LED            20 /* bool: switch_led */
#define DPID_WORK_MODE             21 /* enum: work_mode (white, colour, scene, music) */
#define DPID_BRIGHT_VALUE          22 /* value: bright_value (10-1000) */
#define DPID_TEMP_VALUE            23 /* value: temp_value (0-1000) */
#define DPID_SCENE_DATA            25 /* str: scene_data */
#define DPID_COUNTDOWN             26 /* value: countdown (0-86400, unit: s) */
#define DPID_SLEEP_MODE            31 /* raw: sleep_mode (灯光助眠) */
#define DPID_POWER_MEMORY          33 /* raw: power_memory (断电记忆) */
#define DPID_DO_NOT_DISTURB         34 /* bool: do_not_disturb (停电勿扰) */
#define DPID_RANDOM_TIMING         210 /* raw: random_timing (灯光看家) */
#define BRIGHT_VALUE_MIN           10
#define BRIGHT_VALUE_MAX           1000
#define TEMP_VALUE_MIN             0
#define TEMP_VALUE_MAX             1000
#define KV_KEY_POWER_MEMORY_CFG    "power_memory_cfg"
#define POWER_MEMORY_KV_MAGIC      0xA5
#define KV_KEY_REALTIME_LIGHT      "realtime_light"
#define REALTIME_LIGHT_MAGIC       0xB6
#define KV_KEY_FADE_GRADIENT       "fade_gradient"  /* 渐变时间KV键 */
#define FADE_GRADIENT_MAGIC        0xC7
#define REALTIME_LIGHT_SAVE_MIN_INTERVAL_MS 500
#define COUNTDOWN_MIN              0
#define COUNTDOWN_MAX              86400
#define SCENE_DATA_STR_MAX_LEN     512

/* 自定义功能DP (根据用户提供的JSON定义) */
#define DPID_SCENE_ID              24 /* value: scene_id (0-255) - 自定义功能 */
#define DPID_CYCLE_DATA            27 /* str: cycle_data (group, JSON string) - 自定义功能 */
#define SCENE_ID_MIN               0
#define SCENE_ID_MAX               255
#define CYCLE_DATA_STR_MAX_LEN     512

/***********************************************************/
/* *********************variable define********************* */
/***********************************************************/
STATIC BOOL_T s_switch_state = FALSE;
STATIC UINT8_T s_work_mode = 0;  /* 0=white, 1=colour, 2=scene, 3=music */
STATIC UINT16_T s_brightness_value = (BRIGHT_VALUE_MAX / 2);  /* 默认50%亮度 (500) */
STATIC UINT16_T s_temp_value = (TEMP_VALUE_MAX / 2);  /* 默认50%色温 (500) */
STATIC CHAR_T s_scene_data_str[SCENE_DATA_STR_MAX_LEN + 1] = {0};
STATIC UINT32_T s_countdown = 0;
STATIC THREAD_HANDLE s_countdown_thread = NULL;
STATIC volatile BOOL_T s_countdown_active = FALSE;
STATIC volatile UINT32_T s_countdown_remaining = 0;
STATIC volatile BOOL_T s_countdown_target_state = FALSE;  /* 倒计时结束时的目标状态：TRUE=开灯，FALSE=关灯 */
STATIC UINT8_T s_latest_sleep_raw[128] = {0};
STATIC UINT_T s_latest_sleep_raw_len = 0;
/* 助眠计划结构体 */
typedef struct {
    BOOL_T valid;           /* 计划是否有效 */
    UINT8_T version;        /* 版本号 */
    UINT8_T node;           /* 节点编号 (1-4) */
    BOOL_T on_off;          /* 开关 */
    UINT8_T date;           /* 日期设定（周数据） */
    UINT8_T step;           /* 渐变步进值 */
    UINT8_T hour;           /* 起始小时 */
    UINT8_T minute;         /* 起始分钟 */
    UINT8_T H_hundreds;     /* 色调百位 */
    UINT8_T H_tens_ones;    /* 色调十位个位 */
    UINT8_T S;              /* 饱和度 */
    UINT8_T V;              /* 明度 */
    UINT8_T B;              /* 亮度百分比 */
    UINT8_T T;              /* 色温百分比 */
    UINT8_T raw_data[13];   /* 原始RAW DP数据 */
} SLEEP_PLAN_T;

#define MAX_SLEEP_PLANS 4   /* 最多支持4个计划 */
STATIC SLEEP_PLAN_T s_sleep_plans[MAX_SLEEP_PLANS] = {{0}};  /* 助眠计划数组 */
STATIC volatile BOOL_T s_sleep_mode_active = FALSE;  /* 当前是否有激活的助眠模式 */
STATIC THREAD_HANDLE s_sleep_check_thread = NULL;  /* 助眠模式检查线程 */
STATIC volatile BOOL_T s_sleep_check_thread_running = FALSE;  /* 检查线程运行标志 */

/* 自定义功能变量 */
STATIC UINT8_T s_scene_id = 0;
/* 初始化cycle_data为空数组JSON，让app能够识别和显示 */
STATIC CHAR_T s_cycle_data_str[CYCLE_DATA_STR_MAX_LEN + 1] = "[]";

/* 断电记忆 (DP 33) 相关变量 */
STATIC BOOL_T s_power_memory_enabled = FALSE;  /* 断电记忆是否已启用（mode=0x01或0x02） */
STATIC BOOL_T s_power_memory_received = FALSE;  /* 是否已收到DP 33（用于上电恢复判断） */
typedef struct {
    UINT8_T magic;
    UINT8_T switch_on;
    UINT8_T mode;
    UINT8_T reserved;
    UINT16_T brightness;
    UINT16_T color_temp;
} POWER_MEMORY_STORAGE_T;

typedef struct {
    BOOL_T valid;
    BOOL_T switch_on;
    UINT8_T mode;
    UINT16_T brightness;
    UINT16_T color_temp;
} POWER_MEMORY_CACHE_T;

STATIC POWER_MEMORY_CACHE_T s_power_memory_cache = {0};
STATIC BOOL_T s_power_memory_cache_loaded = FALSE;
STATIC BOOL_T s_power_memory_synced_runtime = FALSE;  /* 本次上电是否已收到DP33 */
typedef struct {
    UINT8_T magic;
    UINT8_T switch_on;
    UINT16_T brightness;
    UINT16_T color_temp;
} REALTIME_LIGHT_STORAGE_T;
STATIC REALTIME_LIGHT_STORAGE_T s_realtime_light_last = {0};
STATIC BOOL_T s_realtime_light_cache_valid = FALSE;
STATIC BOOL_T s_realtime_light_save_enabled = TRUE;
STATIC UINT32_T s_realtime_light_last_save_ms = 0;
STATIC UINT8_T s_fade_in_out_raw[16] = {0};  /* 渐明渐暗原始数据缓存 */
STATIC UINT_T s_fade_in_out_raw_len = 0;     /* 渐明渐暗数据长度 */
/* 渐变时间存储结构（保存到KV） */
typedef struct {
    UINT8_T magic;           /* 魔数，用于验证数据有效性 */
    UINT16_T fade_in_time;   /* 开灯渐明时间（毫秒） */
    UINT16_T fade_out_time;  /* 关灯渐暗时间（毫秒） */
} FADE_GRADIENT_STORAGE_T;

/* 停电勿扰 (DP 34) 相关变量 */
STATIC BOOL_T s_do_not_disturb = FALSE;
#define POWER_CYCLE_REQUIRED 2            /* 通电勿扰开启时，需要连续上电2次 */
#define KV_KEY_DND_STATE          "dnd_state"  /* 通电勿扰状态KV键 */
#define DND_STATE_MAGIC           0xD5
/* 注意：通电勿扰的上电次数现在使用断电复位的KV（power_cycle），不再使用独立的KV */
#define KV_KEY_POWER_CYCLE_COUNT  "power_cycle"  /* 与断电复位共用同一个KV */
typedef struct {
    UINT8_T magic;
    UINT8_T enabled;  /* 通电勿扰是否启用 */
} DND_STATE_STORAGE_T;
/* 断电计数数据结构（与断电复位共用） */
typedef struct {
    UINT32_T count;          /* 断电计数（连续断电累计） */
} POWER_CYCLE_DATA_T;
/* 注意：不再需要 s_dnd_power_cycle_incremented，因为计数由断电复位模块统一管理 */
STATIC BOOL_T s_dnd_state_loaded = FALSE;
STATIC BOOL_T s_dnd_state_need_save = FALSE;

/* 灯光看家 (DP 210) 相关结构体和变量 */
typedef struct {
    BOOL_T valid;           /* 计划是否有效 */
    UINT8_T version;        /* 版本号 */
    UINT8_T length;         /* 节点长度 */
    UINT8_T on_off;         /* 任务开关：02=关闭，03=开启 */
    UINT8_T week;           /* 周数据 */
    UINT16_T start_time;     /* 起始时间（分钟，0-1439） */
    UINT16_T end_time;      /* 结束时间（分钟，0-1439） */
    UINT8_T H_hundreds;     /* 色调百位 (0-3) */
    UINT8_T H_tens_ones;    /* 色调十位个位 (0-99) */
    UINT8_T S;              /* 饱和度百分比 (0-100) */
    UINT8_T V;              /* 明度百分比 (0-100) */
    UINT8_T B;              /* 亮度百分比 (0-100) */
    UINT8_T T;              /* 色温百分比 (0-100) */
    UINT8_T raw_data[128];  /* 原始RAW DP数据 */
    UINT_T raw_data_len;    /* 原始数据长度 */
} RANDOM_TIMING_PLAN_T;

#define MAX_RANDOM_TIMING_PLANS 4   /* 最多支持4个计划 */
STATIC RANDOM_TIMING_PLAN_T s_random_timing_plans[MAX_RANDOM_TIMING_PLANS] = {{0}};  /* 支持多个计划 */
STATIC UINT_T s_random_timing_plan_count = 0;  /* 当前保存的计划数量 */
STATIC RANDOM_TIMING_PLAN_T s_random_timing_plan = {0};  /* 当前激活的计划（用于兼容） */
STATIC volatile BOOL_T s_random_timing_active = FALSE;  /* 当前是否有激活的看家模式 */
STATIC UINT32_T s_once_mode_executed_date = 0;  /* "仅一次"模式执行日期（格式：YYYYMMDD，0表示未执行） */
STATIC THREAD_HANDLE s_random_timing_thread = NULL;
STATIC volatile BOOL_T s_random_timing_thread_running = FALSE;
STATIC volatile UINT32_T s_user_manual_control_time = 0;  /* 用户手动控制时间戳（毫秒），0表示无手动控制 */
#define USER_MANUAL_CONTROL_PAUSE_MS 60000  /* 用户手动控制后，暂停看家灯光60秒 */

/***********************************************************/
/* *******************external declaration****************** */
/***********************************************************/
extern VOID light_control_set(BOOL_T on);
extern VOID light_control_set_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);

/* 函数声明 */
STATIC VOID __random_timing_task(VOID_T *arg);
STATIC VOID __sleep_check_task(VOID_T *arg);
extern BOOL_T light_control_get(VOID);
extern VOID light_control_set_brightness(UINT16_T value);
extern UINT16_T light_control_get_brightness(VOID);
extern VOID light_control_set_color_temp(UINT16_T value);
extern UINT16_T light_control_get_color_temp(VOID);

/* 通电勿扰：检查上电次数，返回是否需要延迟初始化（内部函数） */
STATIC BOOL_T __check_do_not_disturb_power_cycle(VOID);
extern VOID light_control_update_switch_gradient(UINT32_T on_time_ms, UINT32_T off_time_ms);

/***********************************************************/
/* *********************function define********************* */
/***********************************************************/
/* 函数声明 */
STATIC VOID __report_dp_states(BOOL_T is_query);
STATIC VOID __report_all_sleep_plans(VOID);
STATIC BOOL_T __check_week_match(UINT8_T week);
STATIC UINT16_T __get_current_time_minutes(VOID);
STATIC VOID __power_memory_load_cache(VOID);
STATIC VOID __power_memory_save_cache(VOID);
VOID __fade_gradient_load_from_kv(VOID);  /* 从KV加载渐变时间（供外部调用） */
STATIC VOID __power_memory_cache_settings(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp, UINT8_T mode);
STATIC VOID __power_memory_clear_cached(VOID);
STATIC BOOL_T __load_realtime_light_state(REALTIME_LIGHT_STORAGE_T *out);
STATIC VOID __apply_light_state_with_fade(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);

/**
 * @brief 倒计时线程函数
 */
STATIC VOID_T __countdown_task(VOID_T *arg)
{
    while (s_countdown_active && s_countdown_remaining > 0) {
        tal_system_sleep(1000);  /* 睡眠1秒 */
        
            if (s_countdown_active && s_countdown_remaining > 0) {
                s_countdown_remaining--;
                TAL_PR_DEBUG("Countdown remaining: %d seconds", s_countdown_remaining);
                
                if (s_countdown_remaining == 0) {
                    /* 倒计时结束，根据目标状态执行开关灯操作 */
                    TAL_PR_NOTICE("Countdown finished, target state: %s", s_countdown_target_state ? "ON" : "OFF");
                    
                    /* 如果是在助眠模式下，倒计时结束时关闭所有助眠计划 */
                    if (s_sleep_mode_active) {
                        TAL_PR_NOTICE("[SLEEP] Countdown finished in sleep mode, closing all sleep plans");
                        for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
                            SLEEP_PLAN_T *plan = &s_sleep_plans[i];
                            if (plan->valid && plan->on_off) {
                                plan->on_off = FALSE;
                                TAL_PR_NOTICE("[SLEEP] Plan #%d closed (countdown finished)", i);
                            }
                        }
                    }
                    
                    light_control_set(s_countdown_target_state);
                    s_switch_state = s_countdown_target_state;
                    s_countdown = 0;
                    s_countdown_active = FALSE;
                    s_sleep_mode_active = FALSE;  /* 取消助眠模式标记 */
                    
                    /* 上报状态 */
                    __report_dp_states(s_countdown_target_state);
                    break;
                }
            }
    }
    
    s_countdown_thread = NULL;
    TAL_PR_DEBUG("Countdown thread exited");
}

/**
 * @brief 启动倒计时
 */
STATIC VOID __start_countdown(UINT32_T seconds)
{
    /* 停止之前的倒计时 */
    if (s_countdown_thread != NULL) {
        s_countdown_active = FALSE;
        tal_thread_delete(s_countdown_thread);
        s_countdown_thread = NULL;
    }
    
    if (seconds > 0) {
        s_countdown_remaining = seconds;
        s_countdown_active = TRUE;
        
        /* 获取当前灯光状态，倒计时结束时执行相反操作 */
        BOOL_T current_light_state = light_control_get();
        s_countdown_target_state = !current_light_state;  /* 取反：如果当前是关的，目标就是开；如果当前是开的，目标就是关 */
        TAL_PR_NOTICE("Countdown started: %d seconds, current light: %s, target: %s", 
                     seconds, current_light_state ? "ON" : "OFF", s_countdown_target_state ? "ON" : "OFF");
        
        THREAD_CFG_T countdown_cfg = {
            .stackDepth = 2048,
            .priority   = THREAD_PRIO_6,
            .thrdname   = "countdown"
        };
        
        OPERATE_RET ret = tal_thread_create_and_start(&s_countdown_thread, NULL, NULL, 
                                                       __countdown_task, NULL, &countdown_cfg);
        if (ret != OPRT_OK) {
            TAL_PR_ERR("Failed to create countdown thread: %d", ret);
            s_countdown_active = FALSE;
            s_countdown_remaining = 0;
        } else {
            TAL_PR_NOTICE("Countdown thread created successfully");
        }
    } else {
        /* 倒计时为0，停止倒计时 */
        s_countdown_active = FALSE;
        s_countdown_remaining = 0;
        TAL_PR_NOTICE("Countdown stopped (set to 0)");
    }
}

STATIC VOID __power_memory_save_cache(VOID)
{
    POWER_MEMORY_STORAGE_T storage = {0};

    if (s_power_memory_cache.valid) {
        storage.magic = POWER_MEMORY_KV_MAGIC;
        storage.switch_on = s_power_memory_cache.switch_on ? 1 : 0;
        storage.mode = s_power_memory_cache.mode;
        storage.brightness = s_power_memory_cache.brightness;
        storage.color_temp = s_power_memory_cache.color_temp;
    }

    wd_common_write(KV_KEY_POWER_MEMORY_CFG, (BYTE_T *)&storage, sizeof(storage));
}

STATIC VOID __power_memory_load_cache(VOID)
{
    if (s_power_memory_cache_loaded) {
        return;
    }

    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET rt = wd_common_read(KV_KEY_POWER_MEMORY_CFG, &buf, &len);
    if (rt == OPRT_OK && buf != NULL && len == sizeof(POWER_MEMORY_STORAGE_T)) {
        POWER_MEMORY_STORAGE_T storage = {0};
        memcpy(&storage, buf, sizeof(storage));
        if (storage.magic == POWER_MEMORY_KV_MAGIC) {
            s_power_memory_cache.valid = TRUE;
            s_power_memory_cache.switch_on = storage.switch_on ? TRUE : FALSE;
            s_power_memory_cache.mode = storage.mode;
            s_power_memory_cache.brightness = storage.brightness;
            s_power_memory_cache.color_temp = storage.color_temp;
            s_power_memory_enabled = TRUE;
            s_power_memory_received = TRUE;
            TAL_PR_NOTICE("[POWER_MEMORY] Loaded cached cfg from KV (mode=0x%02x, switch=%d, brightness=%d, temp=%d)",
                          s_power_memory_cache.mode,
                          s_power_memory_cache.switch_on,
                          s_power_memory_cache.brightness,
                          s_power_memory_cache.color_temp);
        } else {
            s_power_memory_cache.valid = FALSE;
        }
    } else {
        s_power_memory_cache.valid = FALSE;
    }

    if (buf) {
        wd_common_free_data(buf);
        buf = NULL;
    }

    s_power_memory_cache_loaded = TRUE;
}

STATIC VOID __power_memory_clear_cached(VOID)
{
    memset(&s_power_memory_cache, 0, sizeof(s_power_memory_cache));
    s_power_memory_cache_loaded = TRUE;
    s_power_memory_enabled = FALSE;
    s_power_memory_received = FALSE;

    POWER_MEMORY_STORAGE_T storage = {0};
    wd_common_write(KV_KEY_POWER_MEMORY_CFG, (BYTE_T *)&storage, sizeof(storage));
}

STATIC VOID __power_memory_cache_settings(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp, UINT8_T mode)
{
    if (!switch_on) {
        brightness = 0;
    } else {
        if (brightness < BRIGHT_VALUE_MIN) {
            brightness = BRIGHT_VALUE_MIN;
        }
        if (brightness > BRIGHT_VALUE_MAX) {
            brightness = BRIGHT_VALUE_MAX;
        }
    }

    if (color_temp > TEMP_VALUE_MAX) {
        color_temp = TEMP_VALUE_MAX;
    }

    s_power_memory_cache.switch_on = switch_on ? TRUE : FALSE;
    s_power_memory_cache.mode = mode;
    s_power_memory_cache.brightness = brightness;
    s_power_memory_cache.color_temp = color_temp;
    s_power_memory_cache.valid = TRUE;
    s_power_memory_cache_loaded = TRUE;

    s_power_memory_enabled = TRUE;
    s_power_memory_received = TRUE;

    __power_memory_save_cache();

    TAL_PR_NOTICE("[POWER_MEMORY] Cached settings (mode=0x%02x, switch=%d, brightness=%d, temp=%d)",
                  mode, switch_on, brightness, color_temp);
}

STATIC VOID __apply_light_state_with_fade(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp)
{
    /* 使用批量设置函数，避免连续多次启动渐变导致状态混乱
     * 这样可以确保只启动一次渐变，避免看门狗复位 */
    light_control_set_state(switch_on, brightness, color_temp);
}

/**
 * @brief 读取断电计数（与断电复位共用同一个KV）
 * @return 断电计数
 */
STATIC UINT32_T __dnd_get_power_cycle_count(VOID)
{
    POWER_CYCLE_DATA_T cycle_data = {0};
    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET rt = wd_common_read(KV_KEY_POWER_CYCLE_COUNT, &buf, &len);
    
    if (rt == OPRT_OK && buf != NULL && len == sizeof(cycle_data)) {
        memcpy(&cycle_data, buf, sizeof(cycle_data));
        wd_common_free_data(buf);
        return cycle_data.count;
    }

    if (buf) {
        wd_common_free_data(buf);
    }

    return 0;  /* 如果KV中没有数据，返回0 */
}

/**
 * @brief 清零断电计数（与断电复位共用同一个KV）
 * @note 仅在通电勿扰状态变化时调用，清零计数
 */
STATIC VOID __dnd_power_cycle_reset(VOID)
{
    POWER_CYCLE_DATA_T cycle_data = {0};
    memset(&cycle_data, 0, sizeof(cycle_data));
    wd_common_write(KV_KEY_POWER_CYCLE_COUNT, (BYTE_T *)&cycle_data, sizeof(cycle_data));
    TAL_PR_NOTICE("[DO_NOT_DISTURB] Power cycle count reset (using shared KV with power cycle reset)");
}

/**
 * @brief 延迟保存通电勿扰的上电次数（已废弃，保留接口以兼容）
 * @note 现在使用断电复位的KV，不需要单独保存
 */
VOID dp_dnd_power_cycle_delayed_save(VOID)
{
    /* 已废弃：现在使用断电复位的KV，由断电复位模块统一管理 */
    /* 保留此函数以避免编译错误，但不再执行任何操作 */
}

STATIC BOOL_T __load_realtime_light_state(REALTIME_LIGHT_STORAGE_T *out)
{
    if (out == NULL) {
        return FALSE;
    }

    if (s_realtime_light_cache_valid) {
        memcpy(out, &s_realtime_light_last, sizeof(REALTIME_LIGHT_STORAGE_T));
        return (out->magic == REALTIME_LIGHT_MAGIC);
    }

    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET rt = wd_common_read(KV_KEY_REALTIME_LIGHT, &buf, &len);
    if (rt != OPRT_OK || buf == NULL || len != sizeof(REALTIME_LIGHT_STORAGE_T)) {
        if (buf) {
            wd_common_free_data(buf);
        }
        return FALSE;
    }

    memcpy(&s_realtime_light_last, buf, sizeof(REALTIME_LIGHT_STORAGE_T));
    wd_common_free_data(buf);

    s_realtime_light_cache_valid = (s_realtime_light_last.magic == REALTIME_LIGHT_MAGIC);
    if (!s_realtime_light_cache_valid) {
        return FALSE;
    }

    memcpy(out, &s_realtime_light_last, sizeof(REALTIME_LIGHT_STORAGE_T));
    return TRUE;
}

/**
 * @brief 将十六进制字符转换为数值
 * @param c 十六进制字符 ('0'-'9', 'a'-'f', 'A'-'F')
 * @return 数值 (0-15)，如果不是十六进制字符返回-1
 */
STATIC INT_T __hex_char_to_int(CHAR_T c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief 解析十六进制字符串场景数据并应用
 * @param hex_str 十六进制字符串（例如："030e0d000000000000000032012c"）
 * @return OPERATE_RET
 * 
 * 数据格式（根据云端说明）：
 * - scene_id (1字节): 0x00-0xFF (0-255)
 * - transition_interval (1字节): 0x00-0x64 (0-100)
 * - duration (1字节): 0x00-0x64 (0-100)
 * - change_mode (1字节): 0=静态, 1=跳变, 2=渐变
 * - H (2字节): 0x0000-0x0168 (0-360) 色度
 * - S (2字节): 0x0000-0x03E8 (0-1000) 饱和度
 * - V (2字节): 0x0000-0x03E8 (0-1000) 明度
 * - B (2字节): 0x0000-0x03E8 (0-1000) 白光亮度
 * - T (2字节): 0x0000-0x03E8 (0-1000) 色温值
 * 总计：14字节 = 28个十六进制字符
 */
STATIC OPERATE_RET __parse_and_apply_scene_data_hex(const CHAR_T *hex_str)
{
    if (NULL == hex_str || strlen(hex_str) == 0) {
        TAL_PR_DEBUG("scene_data hex string is empty");
        return OPRT_INVALID_PARM;
    }

    TAL_PR_NOTICE("=== Parsing scene_data HEX string ===");
    TAL_PR_NOTICE("Raw HEX: %s", hex_str);
    TAL_PR_NOTICE("HEX length: %d", strlen(hex_str));

    UINT_T hex_len = strlen(hex_str);
    if (hex_len < 28) {
        TAL_PR_ERR("scene_data hex string too short: %d (expected at least 28 chars)", hex_len);
        return OPRT_INVALID_PARM;
    }

    /* 解析十六进制字符串为字节数组 */
    UINT8_T data[14] = {0};
    for (UINT_T i = 0; i < 14 && (i * 2 + 1) < hex_len; i++) {
        INT_T high = __hex_char_to_int(hex_str[i * 2]);
        INT_T low = __hex_char_to_int(hex_str[i * 2 + 1]);
        if (high < 0 || low < 0) {
            TAL_PR_ERR("Invalid hex char at position %d", i * 2);
            return OPRT_INVALID_PARM;
        }
        data[i] = (UINT8_T)((high << 4) | low);
    }

    /* 打印解析后的字节数据 */
    CHAR_T bytes_str[128] = {0};
    UINT_T bytes_str_pos = 0;
    for (UINT_T i = 0; i < 14; i++) {
        if (bytes_str_pos < sizeof(bytes_str) - 8) {
            bytes_str_pos += sprintf(bytes_str + bytes_str_pos, "0x%02x ", data[i]);
        }
    }
    TAL_PR_NOTICE("Parsed bytes: %s", bytes_str);

    /* 解析各个字段 */
    UINT8_T scene_id = data[0];
    UINT8_T transition_interval = data[1];
    UINT8_T duration = data[2];
    UINT8_T change_mode = data[3];
    UINT16_T H = (UINT16_T)((data[4] << 8) | data[5]);
    UINT16_T S = (UINT16_T)((data[6] << 8) | data[7]);
    UINT16_T V = (UINT16_T)((data[8] << 8) | data[9]);
    UINT16_T B = (UINT16_T)((data[10] << 8) | data[11]);  /* 白光亮度 */
    UINT16_T T = (UINT16_T)((data[12] << 8) | data[13]);  /* 色温值 */

    TAL_PR_NOTICE("Scene ID: %d", scene_id);
    TAL_PR_NOTICE("Transition Interval: %d", transition_interval);
    TAL_PR_NOTICE("Duration: %d", duration);
    TAL_PR_NOTICE("Change Mode: %d (0=静态,1=跳变,2=渐变)", change_mode);
    TAL_PR_NOTICE("H (Hue): %d", H);
    TAL_PR_NOTICE("S (Saturation): %d", S);
    TAL_PR_NOTICE("V (Value): %d", V);
    TAL_PR_NOTICE("B (Brightness): %d (original)", B);
    TAL_PR_NOTICE("T (Color Temp): %d (original)", T);

    /* 场景映射（用户确认，色温逻辑已修正：T=0最暖，T=1000最冷）：
     * - scene_id=0: 温馨（B=1000, T=0，最暖，只亮暖灯）
     * - scene_id=1: 阅读（B=1000, T=500，中间值，冷暖均亮）
     * - scene_id=2: 工作（B=1000, T=1000，最冷，只亮冷灯）
     * - scene_id=3: 夜灯（B=20, T=0，最暖，只亮暖灯）
     */

    /* 温馨场景：scene_id = 0
     * 温馨场景：亮度1000（最亮），色温0（最暖，只亮暖灯）
     */
    if (scene_id == 0) {
        B = BRIGHT_VALUE_MAX;  /* 亮度1000，最亮 */
        T = TEMP_VALUE_MIN;  /* 色温0，最暖，只亮暖灯 */
        TAL_PR_NOTICE("Warm scene (ID=0): set B=%d, T=%d (warm only)", B, T);
    }
    /* 阅读场景：scene_id = 1
     * 阅读场景：亮度1000（最亮），色温500（中间值，冷暖均亮）
     */
    else if (scene_id == 1) {
        B = BRIGHT_VALUE_MAX;  /* 亮度1000，最亮 */
        T = (TEMP_VALUE_MAX / 2);  /* 色温500，中间值，冷暖均亮 */
        TAL_PR_NOTICE("Reading scene (ID=1): set B=%d, T=%d (both cold and warm)", B, T);
    }
    /* 工作场景：scene_id = 2
     * 工作场景：亮度1000（最亮），色温1000（最冷，只亮冷灯）
     */
    else if (scene_id == 2) {
        B = BRIGHT_VALUE_MAX;  /* 亮度1000，最亮 */
        T = TEMP_VALUE_MAX;  /* 色温1000，最冷，只亮冷灯 */
        TAL_PR_NOTICE("Work scene (ID=2): set B=%d, T=%d (cold only)", B, T);
    }
    /* 夜灯场景：scene_id = 3
     * 夜灯场景：亮度20，色温0（最暖，只亮暖灯）
     */
    else if (scene_id == 3) {
        B = 20;  /* 亮度20 */
        T = TEMP_VALUE_MIN;  /* 色温0，最暖，只亮暖灯 */
        TAL_PR_NOTICE("Nightlight scene (ID=3): set B=20, T=%d (warm only)", T);
    }

    TAL_PR_NOTICE("B (Brightness): %d (final)", B);
    TAL_PR_NOTICE("T (Color Temp): %d (final)", T);

    BOOL_T has_changes = FALSE;

    /* 应用亮度值 (B字段) */
    if (B > BRIGHT_VALUE_MAX) {
        B = BRIGHT_VALUE_MAX;
    } else if (B < BRIGHT_VALUE_MIN && B > 0) {
        B = BRIGHT_VALUE_MIN;
    }
    if (B != s_brightness_value) {
        s_brightness_value = B;
        light_control_set_brightness(s_brightness_value);
        has_changes = TRUE;
        TAL_PR_NOTICE("Applied brightness: %d", B);
    }

    /* 应用色温值 (T字段) */
    if (T > TEMP_VALUE_MAX) {
        T = TEMP_VALUE_MAX;
    }
    if (T != s_temp_value) {
        s_temp_value = T;
        light_control_set_color_temp(s_temp_value);
        has_changes = TRUE;
        TAL_PR_NOTICE("Applied color temp: %d", T);
    }

    /* 如果亮度或色温不为0，自动开启灯光 */
    if ((B > 0 || T > 0) && !s_switch_state) {
        s_switch_state = TRUE;
        light_control_set(s_switch_state);
        has_changes = TRUE;
        TAL_PR_NOTICE("Auto turned on light (B=%d, T=%d)", B, T);
    }

    /* 保存场景ID */
    s_scene_id = scene_id;

    if (has_changes) {
        TAL_PR_NOTICE("=== Scene applied successfully ===");
    } else {
        TAL_PR_DEBUG("=== Scene data parsed but no changes needed ===");
    }

    return OPRT_OK;
}
STATIC VOID __report_dp_states(BOOL_T is_query)
{
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S dp_arr[9] = {{0}};
    UINT_T dp_count = 0;

    /* 标准功能DP */
    dp_arr[dp_count].dpid = DPID_SWITCH_LED;
    dp_arr[dp_count].type = PROP_BOOL;
    dp_arr[dp_count].value.dp_bool = s_switch_state ? TRUE : FALSE;
    dp_count++;

    dp_arr[dp_count].dpid = DPID_WORK_MODE;
    dp_arr[dp_count].type = PROP_ENUM;
    dp_arr[dp_count].value.dp_enum = s_work_mode;
    dp_count++;

    dp_arr[dp_count].dpid = DPID_BRIGHT_VALUE;
    dp_arr[dp_count].type = PROP_VALUE;
    dp_arr[dp_count].value.dp_value = s_brightness_value;
    dp_count++;

    dp_arr[dp_count].dpid = DPID_TEMP_VALUE;
    dp_arr[dp_count].type = PROP_VALUE;
    dp_arr[dp_count].value.dp_value = s_temp_value;
    dp_count++;

    dp_arr[dp_count].dpid = DPID_SCENE_DATA;
    dp_arr[dp_count].type = PROP_STR;
    dp_arr[dp_count].value.dp_str = s_scene_data_str;
    dp_count++;

    dp_arr[dp_count].dpid = DPID_COUNTDOWN;
    dp_arr[dp_count].type = PROP_VALUE;
    dp_arr[dp_count].value.dp_value = s_countdown;
    dp_count++;

    /* 自定义功能DP */
    dp_arr[dp_count].dpid = DPID_SCENE_ID;
    dp_arr[dp_count].type = PROP_VALUE;
    dp_arr[dp_count].value.dp_value = s_scene_id;
    dp_count++;

    dp_arr[dp_count].dpid = DPID_CYCLE_DATA;
    dp_arr[dp_count].type = PROP_STR;
    dp_arr[dp_count].value.dp_str = s_cycle_data_str;
    dp_count++;

    /* 停电勿扰 (DP 34) */
    dp_arr[dp_count].dpid = DPID_DO_NOT_DISTURB;
    dp_arr[dp_count].type = PROP_BOOL;
    dp_arr[dp_count].value.dp_bool = s_do_not_disturb ? TRUE : FALSE;
    dp_count++;

    /* 调试日志：打印上报的所有DP */
    TAL_PR_NOTICE("=== Reporting %d DP(s) ===", dp_count);
    for (UINT_T i = 0; i < dp_count; i++) {
        TAL_PR_NOTICE("DP[%d]: dpid=%d, type=%d", i, dp_arr[i].dpid, dp_arr[i].type);
        if (dp_arr[i].dpid == DPID_CYCLE_DATA && dp_arr[i].value.dp_str != NULL) {
            TAL_PR_NOTICE("  -> cycle_data value: %s", dp_arr[i].value.dp_str);
        }
    }

    if (is_query) {
        TUYA_CALL_ERR_LOG(dev_query_dp_json_async(NULL, dp_arr, dp_count));
    } else {
        TUYA_CALL_ERR_LOG(dev_report_dp_json_async(NULL, dp_arr, dp_count));
    }
}

VOID dp_obj_process(CONST TY_OBJ_DP_S *dp_data_arr, UINT_T dp_cnt)
{
    if ((NULL == dp_data_arr) || (0 == dp_cnt)) {
        TAL_PR_ERR("dp_obj_process invalid param");
        return;
    }

    BOOL_T need_report = FALSE;

    /* ========== 调试日志：打印接收到的所有DP数据 ========== */
    TAL_PR_NOTICE("========================================");
    TAL_PR_NOTICE("=== Received %d DP(s) from Cloud/APP ===", dp_cnt);
    TAL_PR_NOTICE("========================================");
    for (UINT_T i = 0; i < dp_cnt; i++) {
        CONST TY_OBJ_DP_S *dp = &dp_data_arr[i];
        TAL_PR_NOTICE("DP[%d]: dpid=%d, type=%d", i, dp->dpid, dp->type);
        
        switch (dp->type) {
            case PROP_BOOL:
                TAL_PR_NOTICE("  -> bool value: %d", dp->value.dp_bool);
                break;
            case PROP_VALUE:
                TAL_PR_NOTICE("  -> value: %d", dp->value.dp_value);
                break;
            case PROP_ENUM:
                TAL_PR_NOTICE("  -> enum: %d", dp->value.dp_enum);
                break;
            case PROP_STR:
                if (dp->value.dp_str != NULL) {
                    UINT_T str_len = strlen(dp->value.dp_str);
                    TAL_PR_NOTICE("  -> str (len=%d): %s", str_len, dp->value.dp_str);
                    /* 如果字符串很长，也打印前100个字符用于调试 */
                    if (str_len > 100) {
                        CHAR_T preview[101] = {0};
                        memcpy(preview, dp->value.dp_str, 100);
                        TAL_PR_NOTICE("  -> str preview (first 100 chars): %s...", preview);
                    }
                } else {
                    TAL_PR_NOTICE("  -> str: NULL");
                }
                break;
            default:
                TAL_PR_NOTICE("  -> unknown type");
            break;
        }
    }
    TAL_PR_NOTICE("========================================");

    for (UINT_T i = 0; i < dp_cnt; i++) {
        CONST TY_OBJ_DP_S *dp = &dp_data_arr[i];

        switch (dp->dpid) {
            case DPID_SWITCH_LED: {
                if (light_control_network_indicator_active()) {
                    TAL_PR_NOTICE("switch_led ignored (network indicator active)");
                    need_report = TRUE;
                    break;
                }
                BOOL_T new_state = dp->value.dp_bool ? TRUE : FALSE;
                TAL_PR_NOTICE("switch_led -> %d", new_state);
                s_switch_state = new_state;
                light_control_set(s_switch_state);
                /* 如果看家灯光激活，记录用户手动控制时间，暂停看家灯光一段时间 */
                if (s_random_timing_active) {
                    s_user_manual_control_time = tal_system_get_millisecond();
                    TAL_PR_NOTICE("[DP210] User manual control detected (switch), pausing random timing for %d seconds", 
                                 USER_MANUAL_CONTROL_PAUSE_MS / 1000);
                }
                need_report = TRUE;
                break;
            }
            case DPID_BRIGHT_VALUE: {
                /* 如果助眠模式激活，忽略亮度调整（避免覆盖助眠设置） */
                if (s_sleep_mode_active) {
                    TAL_PR_DEBUG("bright_value ignored (sleep mode active)");
            break;
                }
                if (light_control_network_indicator_active()) {
                    TAL_PR_NOTICE("bright_value ignored (network indicator active)");
                    need_report = TRUE;
                    break;
                }
                INT_T value = dp->value.dp_value;
                if (value < BRIGHT_VALUE_MIN) {
                    value = BRIGHT_VALUE_MIN;
                } else if (value > BRIGHT_VALUE_MAX) {
                    value = BRIGHT_VALUE_MAX;
                }
                TAL_PR_NOTICE("bright_value -> %d", value);
                s_brightness_value = (UINT16_T)value;
                light_control_set_brightness(s_brightness_value);
                /* 如果看家灯光激活，记录用户手动控制时间，暂停看家灯光一段时间 */
                if (s_random_timing_active) {
                    s_user_manual_control_time = tal_system_get_millisecond();
                    TAL_PR_NOTICE("[DP210] User manual control detected (brightness), pausing random timing for %d seconds", 
                                 USER_MANUAL_CONTROL_PAUSE_MS / 1000);
                }
                need_report = TRUE;
            break;
            }
            case DPID_TEMP_VALUE: {
                /* 如果助眠模式激活，忽略色温调整（避免覆盖助眠设置） */
                if (s_sleep_mode_active) {
                    TAL_PR_DEBUG("temp_value ignored (sleep mode active)");
                    break;
                }
                if (light_control_network_indicator_active()) {
                    TAL_PR_NOTICE("temp_value ignored (network indicator active)");
                    need_report = TRUE;
                    break;
                }
                INT_T value = dp->value.dp_value;
                if (value < TEMP_VALUE_MIN) {
                    value = TEMP_VALUE_MIN;
                } else if (value > TEMP_VALUE_MAX) {
                    value = TEMP_VALUE_MAX;
                }
                TAL_PR_NOTICE("temp_value -> %d", value);
                s_temp_value = (UINT16_T)value;
                light_control_set_color_temp(s_temp_value);
                /* 如果看家灯光激活，记录用户手动控制时间，暂停看家灯光一段时间 */
                if (s_random_timing_active) {
                    s_user_manual_control_time = tal_system_get_millisecond();
                    TAL_PR_NOTICE("[DP210] User manual control detected (color temp), pausing random timing for %d seconds", 
                                 USER_MANUAL_CONTROL_PAUSE_MS / 1000);
                }
                need_report = TRUE;
                break;
            }
            case DPID_WORK_MODE: {
                UINT_T mode = dp->value.dp_enum;
                if (mode > 3) {
                    mode = 0;  /* 默认white模式 */
                }
                TAL_PR_NOTICE("work_mode -> %d (0=white,1=colour,2=scene,3=music)", mode);
                s_work_mode = (UINT8_T)mode;
                /* TODO: 处理工作模式变化 */
                need_report = TRUE;
                break;
            }
            case DPID_SCENE_DATA: {
                if (light_control_network_indicator_active()) {
                    TAL_PR_NOTICE("scene_data ignored (network indicator active)");
                    need_report = TRUE;
                    break;
                }
                if (dp->value.dp_str != NULL) {
                    UINT_T len = strlen(dp->value.dp_str);
                    if (len > SCENE_DATA_STR_MAX_LEN) {
                        len = SCENE_DATA_STR_MAX_LEN;
                        TAL_PR_WARN("scene_data string too long, truncated to %d", len);
                    }
                    memset(s_scene_data_str, 0, sizeof(s_scene_data_str));
                    memcpy(s_scene_data_str, dp->value.dp_str, len);
                    TAL_PR_NOTICE("scene_data -> %s", s_scene_data_str);
                    /* 解析场景数据（十六进制字符串格式）并应用场景 */
                    __parse_and_apply_scene_data_hex(s_scene_data_str);
                } else {
                    memset(s_scene_data_str, 0, sizeof(s_scene_data_str));
                    TAL_PR_NOTICE("scene_data -> NULL (cleared)");
                }
                need_report = TRUE;
                break;
            }
            case DPID_COUNTDOWN: {
                INT_T value = dp->value.dp_value;
                if (value < COUNTDOWN_MIN) {
                    value = COUNTDOWN_MIN;
                } else if (value > COUNTDOWN_MAX) {
                    value = COUNTDOWN_MAX;
                }
                TAL_PR_NOTICE("countdown -> %d seconds", value);
                s_countdown = (UINT32_T)value;
                /* 启动倒计时功能 */
                __start_countdown(s_countdown);
                need_report = TRUE;
                break;
            }
            /* 自定义功能DP */
            case DPID_SCENE_ID: {
                INT_T value = dp->value.dp_value;
                if (value < SCENE_ID_MIN) {
                    value = SCENE_ID_MIN;
                } else if (value > SCENE_ID_MAX) {
                    value = SCENE_ID_MAX;
                }
                TAL_PR_NOTICE("scene_id (custom) -> %d", value);
                s_scene_id = (UINT8_T)value;
                /* TODO: 处理场景ID变化，例如加载对应场景配置 */
                need_report = TRUE;
                break;
            }
            case DPID_CYCLE_DATA: {
                if (dp->value.dp_str != NULL) {
                    UINT_T len = strlen(dp->value.dp_str);
                    if (len > CYCLE_DATA_STR_MAX_LEN) {
                        len = CYCLE_DATA_STR_MAX_LEN;
                        TAL_PR_WARN("cycle_data string too long, truncated to %d", len);
                    }
                    memset(s_cycle_data_str, 0, sizeof(s_cycle_data_str));
                    memcpy(s_cycle_data_str, dp->value.dp_str, len);
                    TAL_PR_NOTICE("cycle_data (custom) -> %s", s_cycle_data_str);
                    /* TODO: 解析JSON字符串，提取子字段并处理循环数据 */
                    /* 子字段包括：
                     *   - transition_interval (0-100)
                     *   - duration (0-100)
                     *   - change_mode (0/1/2)
                     *   - H (0-360)
                     *   - S (0-1000)
                     *   - V (0-1000)
                     *   - B (0-1000)
                     *   - T (0-1000)
                     */
                } else {
                    memset(s_cycle_data_str, 0, sizeof(s_cycle_data_str));
                    TAL_PR_NOTICE("cycle_data -> NULL (cleared)");
                }
                need_report = TRUE;
            break;
            }
            case DPID_DO_NOT_DISTURB: {
                BOOL_T new_state = dp->value.dp_bool ? TRUE : FALSE;
                BOOL_T old_state = s_do_not_disturb;
                TAL_PR_NOTICE("do_not_disturb -> %d", new_state);

                if (new_state != old_state) {
                    s_do_not_disturb = new_state;
                    /* 确保状态已加载标记，避免后续加载覆盖新设置的值 */
                    if (!s_dnd_state_loaded) {
                        s_dnd_state_loaded = TRUE;
                    }
                    /* 标记需要保存通电勿扰状态到KV，确保断电后状态不丢失
                     * 使用延迟保存，避免在DP处理的关键路径上立即写入KV导致看门狗复位 */
                    s_dnd_state_need_save = TRUE;
                    /* 只有在状态变化时才清零上电计数 */
                    __dnd_power_cycle_reset();
                    TAL_PR_NOTICE("[DO_NOT_DISTURB] State changed, power-cycle counter reset");
                } else {
                    TAL_PR_NOTICE("[DO_NOT_DISTURB] State unchanged, keep current power-cycle counter");
                }

                /* 延迟保存会在DP处理完成后统一执行 */
                need_report = TRUE;
                
                /* 注意：通电勿扰的上电次数检查在 user_main 中执行
                 * 运行时收到DP 34时，只更新状态，不检查上电次数
                 * 状态保存会在DP处理完成后统一执行
                 */
                break;
            }
            default: 
                TAL_PR_DEBUG("Unhandled obj dpid:%d", dp->dpid);
            break;
        }
    }

    if (need_report) {
        __report_dp_states(FALSE);
    }
    
    /* DP处理完成后，如果标记了需要保存通电勿扰状态，立即保存
     * 此时不在关键路径上，可以安全地保存 */
    if (s_dnd_state_need_save) {
        dp_dnd_state_delayed_save();
    }
}

/**
 * @brief 上报所有有效的助眠计划
 * 将所有有效计划的数据合并成一个RAW DP数据上报
 * 格式：[version(1字节)] [node_count(1字节)] [plan1(13字节)] [plan2(13字节)] ...
 */
STATIC VOID __report_all_sleep_plans(VOID)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_latest_sleep_raw_len > 0) {
        TAL_PR_NOTICE("Re-reporting cached sleep mode RAW (%d bytes)", s_latest_sleep_raw_len);
        TUYA_CALL_ERR_LOG(dev_report_dp_raw_sync(NULL, DPID_SLEEP_MODE, s_latest_sleep_raw, s_latest_sleep_raw_len, 5));
    } else {
        TAL_PR_NOTICE("No cached sleep mode RAW, reporting empty payload");
        UINT8_T empty_data[2] = {0x00, 0x00};
        TUYA_CALL_ERR_LOG(dev_report_dp_raw_sync(NULL, DPID_SLEEP_MODE, empty_data, sizeof(empty_data), 5));
    }
}

VOID dp_raw_process(UINT8_T dpid, CONST UINT8_T *p_data, UINT_T data_len)
{
    /* ========== 调试日志：打印接收到的Raw DP数据 ========== */
    /* 使用单行日志减少输出量，避免缓冲区溢出 */
    TAL_PR_NOTICE("[RAW_DP] ENTRY: dpid=%d(0x%02x) len=%d ptr=%p", dpid, dpid, data_len, (VOID*)p_data);
    
    if (NULL == p_data || 0 == data_len) {
        TAL_PR_ERR("RAW DP data is NULL or empty");
        TAL_PR_NOTICE("========================================");
        return;
    }

    /* 打印十六进制数据（简化输出，避免缓冲区溢出） */
    if (data_len > 0) {
        CHAR_T hex_str[96] = {0};  /* 减少缓冲区大小 */
        UINT_T print_len = (data_len > 16) ? 16 : data_len;  /* 只打印前16字节 */
        for (UINT_T i = 0; i < print_len; i++) {
            sprintf(hex_str + i * 3, "%02x ", p_data[i]);
        }
        TAL_PR_NOTICE("[RAW_DP] Data(hex): %s%s", hex_str, (data_len > 16) ? "..." : "");
    }

    /* 如果是场景相关的Raw DP，尝试解析 */
    if (dpid == DPID_SCENE_DATA || dpid == DPID_CYCLE_DATA) {
        TAL_PR_NOTICE("This is scene/cycle related RAW DP, but currently not parsed");
    }
    
    /* 断电记忆功能 (DPID 33) */
    if (dpid == DPID_POWER_MEMORY) {
        TAL_PR_NOTICE("=== Parsing Power Memory RAW DP ===");
        
        if (data_len < 12) {
            TAL_PR_ERR("Power memory RAW DP data too short: %d (expected at least 12 bytes)", data_len);
            TAL_PR_NOTICE("========================================");
            return;
        }
        
        /* 解析数据格式 */
        UINT8_T version = p_data[0];
        UINT8_T mode = p_data[1];
        UINT16_T H = (UINT16_T)((p_data[2] << 8) | p_data[3]);
        UINT16_T S = (UINT16_T)((p_data[4] << 8) | p_data[5]);
        UINT16_T V = (UINT16_T)((p_data[6] << 8) | p_data[7]);
        UINT16_T B = (UINT16_T)((p_data[8] << 8) | p_data[9]);
        UINT16_T T = (UINT16_T)((p_data[10] << 8) | p_data[11]);
        
        TAL_PR_NOTICE("Power Memory - Version: 0x%02x, Mode: 0x%02x, H:%d S:%d V:%d B:%d T:%d",
                      version, mode, H, S, V, B, T);
        
        BOOL_T switch_on = FALSE;
        UINT16_T target_brightness = 0;
        UINT16_T target_temp = 0;
        
        switch (mode) {
            case 0x00: { /* 固定 50% 亮度、50% 色温 */
                switch_on = TRUE;
                target_brightness = BRIGHT_VALUE_MAX / 2;
                target_temp = TEMP_VALUE_MAX / 2;
                TAL_PR_NOTICE("[POWER_MEMORY] Mode=0x00 -> fixed 50%% brightness & color temp");
                break;
            }
            case 0x01: { /* 记录当前灯光状态 */
                BOOL_T current_on = light_control_get();
                UINT16_T current_brightness = light_control_get_brightness();
                UINT16_T current_temp = light_control_get_color_temp();
                switch_on = current_on;
                target_brightness = current_on ? current_brightness : 0;
                target_temp = current_temp ? current_temp : (TEMP_VALUE_MAX / 2);
                TAL_PR_NOTICE("[POWER_MEMORY] Mode=0x01 -> cache current light state (on=%d, brightness=%d, temp=%d)",
                              current_on, current_brightness, current_temp);
                break;
            }
            case 0x02: { /* 使用RAW中的自定义参数 */
                BOOL_T use_hsv_mode = (B == 0 && V > 0);
                if (use_hsv_mode) {
                    target_brightness = V;
                    target_temp = (T == 0) ? (TEMP_VALUE_MAX / 2) : T;
                    TAL_PR_NOTICE("[POWER_MEMORY] Mode=0x02 -> HSV payload: V=%d, T=%d", V, target_temp);
                } else if (B > 0) {
                    target_brightness = B;
                    target_temp = T;
                    TAL_PR_NOTICE("[POWER_MEMORY] Mode=0x02 -> White payload: B=%d, T=%d", B, T);
                } else {
                    target_brightness = 0;
                    target_temp = 0;
                    TAL_PR_NOTICE("[POWER_MEMORY] Mode=0x02 -> OFF payload (B=0,V=0)");
                }
                switch_on = (target_brightness > 0);
                break;
            }
            default: {
                TAL_PR_WARN("[POWER_MEMORY] Unknown mode=0x%02x, clearing cached settings", mode);
                __power_memory_clear_cached();
                TAL_PR_NOTICE("========================================");
                return;
            }
        }
        
        __power_memory_cache_settings(switch_on, target_brightness, target_temp, mode);
        s_power_memory_synced_runtime = TRUE;
        TAL_PR_NOTICE("[POWER_MEMORY] Cached only (no immediate apply). Will take effect on next power cycle.");
        
        /* 上报RAW DP状态，作为ACK */
        UINT8_T report_data[12] = {0};
        report_data[0] = version;
        report_data[1] = mode;
        report_data[2] = (H >> 8) & 0xFF;
        report_data[3] = H & 0xFF;
        report_data[4] = (S >> 8) & 0xFF;
        report_data[5] = S & 0xFF;
        report_data[6] = (V >> 8) & 0xFF;
        report_data[7] = V & 0xFF;
        report_data[8] = (B >> 8) & 0xFF;
        report_data[9] = B & 0xFF;
        report_data[10] = (T >> 8) & 0xFF;
        report_data[11] = T & 0xFF;
        
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, DPID_POWER_MEMORY, report_data, sizeof(report_data), 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report power_memory: %d", rt);
        } else {
            TAL_PR_DEBUG("Power memory reported successfully");
        }
        
        TAL_PR_NOTICE("========================================");
        return;
    }
    
    /* 灯光助眠功能 (DPID 31) */
    if (dpid == DPID_SLEEP_MODE) {
        TAL_PR_NOTICE("=== Parsing Sleep Mode RAW DP ===");
        if (data_len > sizeof(s_latest_sleep_raw)) {
            TAL_PR_WARN("Sleep mode RAW length %d exceeds cache size %d, truncating", data_len, sizeof(s_latest_sleep_raw));
            s_latest_sleep_raw_len = sizeof(s_latest_sleep_raw);
        } else {
            s_latest_sleep_raw_len = data_len;
        }
        if (s_latest_sleep_raw_len > 0) {
            memcpy(s_latest_sleep_raw, p_data, s_latest_sleep_raw_len);
        }
        
        /* 处理删除命令：2字节格式 [0]=version, [1]=node
         * node=0x00: 删除所有计划
         * node=0x01-0x04: 删除指定节点编号的计划
         */
        if (data_len == 2) {
            UINT8_T version = p_data[0];
            UINT8_T node = p_data[1];
            TAL_PR_NOTICE("Sleep Mode - Delete command: version=%d, node=%d", version, node);
            
            if (node == 0x00) {
                /* 删除所有助眠计划 */
                TAL_PR_NOTICE("Deleting all sleep mode plans...");
                for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
                    s_sleep_plans[i].valid = FALSE;
                }
                s_sleep_mode_active = FALSE;
                __start_countdown(0);  /* 停止倒计时 */
                
                /* 停止检查线程 */
                if (s_sleep_check_thread != NULL) {
                    s_sleep_check_thread_running = FALSE;
                    tal_system_sleep(200);  /* 等待线程退出 */
                    tal_thread_delete(s_sleep_check_thread);
                    s_sleep_check_thread = NULL;
                    TAL_PR_NOTICE("[SLEEP] Check thread stopped");
                }
            } else if (node >= 0x01 && node <= MAX_SLEEP_PLANS) {
                /* 删除指定节点编号的计划 */
                UINT_T plan_idx = node - 1;  /* node 1-4 对应数组索引 0-3 */
                TAL_PR_NOTICE("Deleting sleep mode plan at node %d (index %d)", node, plan_idx);
                s_sleep_plans[plan_idx].valid = FALSE;
                
                /* 如果删除的是当前激活的计划，停止倒计时 */
                if (s_sleep_mode_active && s_sleep_plans[plan_idx].on_off) {
                    s_sleep_mode_active = FALSE;
                    __start_countdown(0);
                }
                
                /* 如果所有计划都关闭了，停止检查线程 */
                BOOL_T has_active_plan = FALSE;
                for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
                    if (s_sleep_plans[i].valid && s_sleep_plans[i].on_off) {
                        has_active_plan = TRUE;
                        break;
                    }
                }
                if (!has_active_plan && s_sleep_check_thread != NULL) {
                    s_sleep_check_thread_running = FALSE;
                    tal_system_sleep(200);  /* 等待线程退出 */
                    tal_thread_delete(s_sleep_check_thread);
                    s_sleep_check_thread = NULL;
                    TAL_PR_NOTICE("[SLEEP] Check thread stopped (no active plans after delete)");
                }
            }
            
            /* 上报删除后的所有计划状态 */
            __report_all_sleep_plans();
            
            /* 上报状态 */
            __report_dp_states(FALSE);
            TAL_PR_NOTICE("========================================");
    return;
}

        if (data_len >= 13) {
            /* 根据云端配置，sleep_mode是group类型RAW DP，字段定义：
             * [0]: version (版本号，枚举，0x00=初始版本)
             * [1]: node (任务节点数，枚举，0x01=1个节点, 0x02=2个节点...)
             * [2]: on_off (任务开关，布尔，0x00=关闭, 0x01=开启)
             * [3]: date (日期设定，周数据，0x00=单次模式)
             * [4]: step (渐变步进值，1-24，5分钟一个步进)
             * [5]: hour (起始小时，0-23)
             * [6]: minute (起始分钟，0-59)
             * [7]: H_hundreds (色调百位，0-3)
             * [8]: H_tens_ones (色调十位个位，0-99)
             * [9]: S (饱和度百分比，0-100)
             * [10]: V (明度百分比，0-100)
             * [11]: B (亮度百分比，0-100)
             * [12]: T (色温百分比，0-100)
             */
            UINT8_T version = p_data[0];
            UINT8_T node = p_data[1];
            UINT8_T on_off = p_data[2];
            UINT8_T date = p_data[3];
            UINT8_T step = p_data[4];
            UINT8_T hour = p_data[5];
            UINT8_T minute = p_data[6];
            UINT8_T H_hundreds = p_data[7];
            UINT8_T H_tens_ones = p_data[8];
            UINT8_T S = p_data[9];
            UINT8_T V = p_data[10];
            UINT8_T B = p_data[11];  /* 亮度百分比 0-100 */
            UINT8_T T = p_data[12];  /* 色温百分比 0-100 */
            
            TAL_PR_NOTICE("Sleep Mode - Version: %d, Node: %d, OnOff: %d", version, node, on_off);
            
            /* 解析Date字段（星期位掩码） */
            CHAR_T week_str[64] = {0};
            if (date == 0) {
                strcpy(week_str, "单次模式");
            } else {
                /* 位掩码：bit0=周日(0x01), bit1=周一(0x02), bit2=周二(0x04), bit3=周三(0x08), 
                 *         bit4=周四(0x10), bit5=周五(0x20), bit6=周六(0x40) */
                BOOL_T has_any = FALSE;
                if (date & 0x01) { strcat(week_str, "周日 "); has_any = TRUE; }
                if (date & 0x02) { strcat(week_str, "周一 "); has_any = TRUE; }
                if (date & 0x04) { strcat(week_str, "周二 "); has_any = TRUE; }
                if (date & 0x08) { strcat(week_str, "周三 "); has_any = TRUE; }
                if (date & 0x10) { strcat(week_str, "周四 "); has_any = TRUE; }
                if (date & 0x20) { strcat(week_str, "周五 "); has_any = TRUE; }
                if (date & 0x40) { strcat(week_str, "周六 "); has_any = TRUE; }
                if (!has_any) {
                    strcpy(week_str, "无星期设置");
                }
            }
            
            TAL_PR_NOTICE("Sleep Mode - Date: 0x%02x (%s), Step: %d, Time: %02d:%02d", 
                          date, week_str, step, hour, minute);
            TAL_PR_NOTICE("Sleep Mode - H: %d%d, S: %d, V: %d, B: %d%%, T: %d%%", 
                          H_hundreds, H_tens_ones, S, V, B, T);
            
            /* 根据 node 字段（1-4）保存到对应的计划槽位 */
            if (node >= 0x01 && node <= MAX_SLEEP_PLANS) {
                UINT_T plan_idx = node - 1;  /* node 1-4 对应数组索引 0-3 */
                SLEEP_PLAN_T *plan = &s_sleep_plans[plan_idx];
                
                /* 保存计划数据 */
                plan->valid = TRUE;
                plan->version = version;
                plan->node = node;
                plan->on_off = (on_off == 0x01) ? TRUE : FALSE;
                plan->date = date;
                plan->step = step;
                plan->hour = hour;
                plan->minute = minute;
                plan->H_hundreds = H_hundreds;
                plan->H_tens_ones = H_tens_ones;
                plan->S = S;
                plan->V = V;
                plan->B = B;
                plan->T = T;
                if (data_len <= sizeof(plan->raw_data)) {
                    memcpy(plan->raw_data, p_data, data_len);
                }
                
                TAL_PR_NOTICE("Sleep mode plan saved: node=%d, index=%d, on_off=%d", node, plan_idx, plan->on_off);
                
                if (on_off == 0x01) {
                    /* 开启助眠模式 - 检查当前时间是否匹配计划时间 */
                    TAL_PR_NOTICE("Sleep mode plan enabled at node %d, scheduled time: %02d:%02d", node, hour, minute);
                    
                    /* 获取当前时间 */
                    UINT16_T current_minutes = __get_current_time_minutes();
                    UINT16_T plan_minutes = (UINT16_T)(hour * 60 + minute);
                    
                    /* 检查当前时间是否匹配计划时间（允许±10秒误差） */
                    INT16_T time_diff = (INT16_T)(current_minutes - plan_minutes);
                    if (time_diff < 0) {
                        time_diff = -time_diff;
                    }
                    BOOL_T time_match = (time_diff == 0);  /* 只允许完全匹配，因为检查间隔已缩短 */
                    
                    /* 检查星期是否匹配（使用统一的检查函数） */
                    BOOL_T week_match = __check_week_match(date);
                    
                    TAL_PR_NOTICE("Sleep mode - Current time: %d(%02d:%02d), Plan time: %d(%02d:%02d), time_match=%d, week_match=%d", 
                                  current_minutes, current_minutes/60, current_minutes%60,
                                  plan_minutes, hour, minute, time_match, week_match);
                    
                    /* 只有在时间匹配且星期匹配时才立即执行 */
                    if (time_match && week_match) {
                        TAL_PR_NOTICE("Sleep mode - Time matched! Applying sleep mode now");
                        s_sleep_mode_active = TRUE;  /* 标记助眠模式激活 */
                        
                        /* 将百分比值转换为0-1000范围
                         * B (亮度): 0-100% -> 0-1000
                         * T (色温): 0-100% -> 0-1000
                         */
                        UINT16_T brightness_value = (UINT16_T)B * 10;  /* 0-100 -> 0-1000 */
                        if (brightness_value < BRIGHT_VALUE_MIN && brightness_value > 0) {
                            brightness_value = BRIGHT_VALUE_MIN;
                        } else if (brightness_value > BRIGHT_VALUE_MAX) {
                            brightness_value = BRIGHT_VALUE_MAX;
                        }
                        
                        UINT16_T temp_value = (UINT16_T)T * 10;  /* 0-100 -> 0-1000 */
                        if (temp_value > TEMP_VALUE_MAX) {
                            temp_value = TEMP_VALUE_MAX;
                        }
                        
                        s_brightness_value = brightness_value;
                        s_temp_value = temp_value;
                        s_switch_state = TRUE;
                        
                        TAL_PR_NOTICE("Sleep mode - Setting light: B=%d (from %d%%), T=%d (from %d%%)", 
                                      brightness_value, B, temp_value, T);
                        
                        light_control_set(s_switch_state);
                        light_control_set_brightness(s_brightness_value);
                        light_control_set_color_temp(s_temp_value);
                        
                        TAL_PR_NOTICE("Sleep mode applied - B: %d, T: %d, Switch: ON", brightness_value, temp_value);
                        
                        /* 计算持续时间：step * 5分钟 = 秒数 */
                        UINT32_T duration = (UINT32_T)step * 5 * 60;  /* step * 5分钟 * 60秒/分钟 */
                        if (duration > 0) {
                            TAL_PR_NOTICE("Sleep mode will last %d seconds (%d steps * 5 minutes)", duration, step);
                            __start_countdown(duration);
                        }
                    } else {
                        /* 时间不匹配，只保存计划，等待到计划时间再执行 */
                        TAL_PR_NOTICE("Sleep mode - Time not matched, plan saved. Will execute at %02d:%02d", hour, minute);
                        s_sleep_mode_active = FALSE;  /* 暂时不激活，等待计划时间 */
                        
                        /* 如果检查线程未运行，启动线程 */
                        if (s_sleep_check_thread == NULL && !s_sleep_check_thread_running) {
                            THREAD_CFG_T thread_cfg = {
                                .thrdname   = "sleep_check",
                                .priority   = THREAD_PRIO_6,
                                .stackDepth = 2048,
                            };
                            OPERATE_RET rt = tal_thread_create_and_start(&s_sleep_check_thread, NULL, NULL, 
                                                                         __sleep_check_task, NULL, &thread_cfg);
                            if (rt != OPRT_OK) {
                                TAL_PR_ERR("[SLEEP] Thread create FAILED: %d", rt);
                            } else {
                                s_sleep_check_thread_running = TRUE;
                                TAL_PR_NOTICE("[SLEEP] Check thread STARTED");
                            }
                        }
                    }
                } else if (on_off == 0x00) {
                    /* 关闭该计划 */
                    TAL_PR_NOTICE("Sleep mode plan disabled at node %d", node);
                    plan->on_off = FALSE;
                    
                    /* 如果关闭的是当前激活的计划，停止倒计时 */
                    if (s_sleep_mode_active) {
                        s_sleep_mode_active = FALSE;
                        __start_countdown(0);
                    }
                    
                    /* 如果所有计划都关闭了，停止检查线程 */
                    BOOL_T has_active_plan = FALSE;
                    for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
                        if (s_sleep_plans[i].valid && s_sleep_plans[i].on_off) {
                            has_active_plan = TRUE;
                            break;
                        }
                    }
                    if (!has_active_plan && s_sleep_check_thread != NULL) {
                        s_sleep_check_thread_running = FALSE;
                        tal_system_sleep(200);  /* 等待线程退出 */
                        tal_thread_delete(s_sleep_check_thread);
                        s_sleep_check_thread = NULL;
                        TAL_PR_NOTICE("[SLEEP] Check thread stopped (no active plans)");
                    }
                }
                
                /* 上报所有计划的状态（合并所有有效计划的数据） */
                __report_all_sleep_plans();
                
                /* 上报状态 */
                __report_dp_states(FALSE);
            } else {
                TAL_PR_ERR("Invalid node value: %d (should be 1-%d)", node, MAX_SLEEP_PLANS);
            }
        } else {
            TAL_PR_ERR("Sleep mode RAW DP data too short: %d (expected at least 13 bytes)", data_len);
        }
    }

    /* 灯光看家功能 (DPID 210) */
    if (dpid == DPID_RANDOM_TIMING) {
        TAL_PR_NOTICE("[DP210] START: len=%d", data_len);
        
        /* 处理删除命令（优先处理，可能在data_len < 2的情况下）：
         * 格式1: 2字节 [0]=version, [1]=length(0x00表示删除所有计划)
         * 格式2: 可能还有其他格式，检查length=0的情况
         * 格式3: 可能只有1字节或空数据
         */
        UINT8_T version = 0;
        UINT8_T length = 0;
        
        if (data_len >= 1) {
            version = p_data[0];
        }
        if (data_len >= 2) {
            length = p_data[1];
        }
        
        TAL_PR_NOTICE("[RAW DP 210] version=0x%02x, length=0x%02x, data_len=%d", 
                      version, length, data_len);
        
        /* 打印完整数据用于调试 */
        if (data_len > 0) {
            CHAR_T hex_str[128] = {0};
            UINT_T print_len = (data_len > 32) ? 32 : data_len;
            for (UINT_T i = 0; i < print_len; i++) {
                if (i * 3 < sizeof(hex_str) - 4) {
                    sprintf(hex_str + i * 3, "%02x ", p_data[i]);
                }
            }
            TAL_PR_NOTICE("[RAW DP 210] Data (hex): %s", hex_str);
            if (data_len > 32) {
                TAL_PR_NOTICE("[RAW DP 210] ... (total %d bytes, showing first 32)", data_len);
            }
        }
        
        /* 检查是否为删除命令：
         * 1. 2字节且length=0x00
         * 2. length=0x00（无论数据长度，只要>=2字节）
         * 3. 只有1字节且version=0x00（可能是特殊删除命令）
         * 4. 数据长度为0（空数据，表示删除）
         * 5. 2字节数据，但length>0且data_len < 2+length（数据不足，可能是删除命令）
         *    例如：0x00 0x0c 表示version=0x00, length=12，但只有2字节数据，实际需要14字节
         */
        BOOL_T is_delete_cmd = FALSE;
        if (data_len == 0) {
            /* 空数据，表示删除 */
            is_delete_cmd = TRUE;
            TAL_PR_NOTICE("Random Timing - Delete command detected: empty data");
        } else if (data_len == 1 && version == 0x00) {
            /* 1字节且version=0x00，可能是删除命令 */
            is_delete_cmd = TRUE;
            TAL_PR_NOTICE("Random Timing - Delete command detected: 1 byte, version=0x00");
        } else if (data_len == 2 && length == 0x00) {
            /* 2字节且length=0x00，标准删除命令 */
            is_delete_cmd = TRUE;
            TAL_PR_NOTICE("Random Timing - Delete command detected: 2 bytes, length=0x00");
        } else if (data_len >= 2 && length == 0x00) {
            /* length=0x00（无论数据长度） */
            is_delete_cmd = TRUE;
            TAL_PR_NOTICE("Random Timing - Delete command detected: length=0x00, data_len=%d", data_len);
        } else if (data_len == 2 && length > 0 && length < 255) {
            /* 2字节数据，但length>0且数据不足（实际需要2+length字节）
             * 例如：0x00 0x0c 表示需要14字节，但只有2字节，这是删除命令的特殊格式
             */
            UINT_T required_len = 2 + length;
            TAL_PR_NOTICE("Random Timing - Checking delete condition: data_len=%d, length=0x%02x, required_len=%d", 
                          data_len, length, required_len);
            if (data_len < required_len) {
                is_delete_cmd = TRUE;
                TAL_PR_NOTICE("Random Timing - Delete command detected: data_len=%d < required=%d (length=0x%02x)", 
                              data_len, required_len, length);
            } else {
                TAL_PR_NOTICE("Random Timing - Not delete command: data_len=%d >= required=%d", 
                              data_len, required_len);
            }
        } else {
            TAL_PR_NOTICE("Random Timing - Delete check failed: data_len=%d, length=0x%02x", data_len, length);
        }
        
        if (is_delete_cmd) {
            TAL_PR_NOTICE("Random Timing - Processing delete all command...");
            
            /* 停止看家模式 */
            s_random_timing_active = FALSE;
            s_random_timing_plan.valid = FALSE;
            s_random_timing_thread_running = FALSE;
            
            /* 停止并删除线程 */
            if (s_random_timing_thread != NULL) {
                TAL_PR_NOTICE("Random Timing - Stopping thread...");
                s_random_timing_thread_running = FALSE;
                tal_system_sleep(200);  /* 等待线程退出 */
                tal_thread_delete(s_random_timing_thread);
                s_random_timing_thread = NULL;
                TAL_PR_NOTICE("Random Timing - Thread stopped");
            }
            
            /* 关闭灯光（如果正在看家模式中） */
            if (light_control_get()) {
                light_control_set(FALSE);
                TAL_PR_NOTICE("Random Timing - Light turned OFF after delete");
            }
            
            /* 清空计划数据 */
            UINT_T deleted_count = s_random_timing_plan_count;
            memset(&s_random_timing_plan, 0, sizeof(s_random_timing_plan));
            s_random_timing_plan.raw_data_len = 0;
            memset(s_random_timing_plans, 0, sizeof(s_random_timing_plans));
            s_random_timing_plan_count = 0;
            s_once_mode_executed_date = 0;  /* 清除"仅一次"模式执行记录 */
            TAL_PR_NOTICE("[DP210] All plans deleted (total was %d)", deleted_count);
            
            /* 上报删除后的状态（空数据：version=0x00, length=0x00） */
            UINT8_T empty_data[2] = {0x00, 0x00};
            OPERATE_RET rt = dev_report_dp_raw_sync(NULL, DPID_RANDOM_TIMING, empty_data, sizeof(empty_data), 5);
            if (rt != OPRT_OK) {
                TAL_PR_ERR("Failed to report deleted random_timing: %d", rt);
            } else {
                TAL_PR_NOTICE("Random timing deleted and reported successfully");
            }
            
            __report_dp_states(FALSE);
            TAL_PR_NOTICE("Random Timing - All plans deleted successfully");
            TAL_PR_NOTICE("========================================");
            return;
        }
        
        /* 如果不是删除命令，继续解析正常数据 */
        
        /* 检查数据长度是否足够 */
        if (data_len < 2) {
            TAL_PR_ERR("Random timing RAW DP data too short: %d (expected at least 2 bytes for plan data)", data_len);
            TAL_PR_NOTICE("========================================");
            return;
        }

        /* 解析数据格式（根据JSON协议定义）：
         * [0]: version (1字节，枚举，0x00)
         * [1]: length (1字节，节点数据长度，不包括version和length本身，通常为12)
         * [2]: on_off (1字节): 02=关闭，03=开启
         * [3]: week (1字节): 周数据
         * [4-5]: start_time (2字节，大端): 起始时间（分钟，0-1439）
         * [6-7]: end_time (2字节，大端): 结束时间（分钟，0-1439）
         * [8]: H_hundreds (1字节): 0-3
         * [9]: H_tens_ones (1字节): 0-99
         * [10]: S (1字节): 0-100
         * [11]: V (1字节): 0-100
         * [12]: B (1字节): 0-100
         * [13]: T (1字节): 0-100
         * 
         * 总长度 = 2字节头部 + length字节数据 = 14字节（当length=12时）
         */
        /* version和length已经在上面读取过了 */
        TAL_PR_NOTICE("Random Timing - Parsing plan data: Version: 0x%02x, Length: %d, Total data_len: %d", 
                      version, length, data_len);
        
        /* length应该是节点数据长度（12字节），不是节点数量 */
        /* 注意：length=0已经在上面作为删除命令处理了，这里不应该再出现 */
        if (length == 0) {
            TAL_PR_ERR("Invalid length value: %d (should be > 0, delete command should be handled earlier)", length);
            TAL_PR_NOTICE("========================================");
            return;
        }

        /* 计算需要的最小数据长度：2字节头部 + length字节节点数据 */
        UINT_T min_data_len = 2 + length;
        if (data_len < min_data_len) {
            TAL_PR_ERR("Random timing RAW DP data too short: %d (expected at least %d bytes)", data_len, min_data_len);
            TAL_PR_NOTICE("========================================");
            return;
        }

        /* 如果length不是12，记录警告但继续解析 */
        if (length != 12) {
            TAL_PR_WARN("Unexpected length value: %d (expected 12), will parse %d bytes", length, length);
        }

        /* 解析节点数据（从offset=2开始） */
        UINT_T offset = 2;
        
        /* 检查是否有足够的数据 */
        if (data_len < offset + length) {
            TAL_PR_ERR("Insufficient data: offset=%d, length=%d, data_len=%d", offset, length, data_len);
            TAL_PR_NOTICE("========================================");
            return;
        }
        
        UINT8_T on_off = p_data[offset + 0];
        UINT8_T week = p_data[offset + 1];
        UINT16_T start_time = 0;
        UINT16_T end_time = 0;
        UINT8_T H_hundreds = 0;
        UINT8_T H_tens_ones = 0;
        UINT8_T S = 0;
        UINT8_T V = 0;
        UINT8_T B = 0;
        UINT8_T T = 0;
        
        /* 根据实际数据长度解析 */
        if (length >= 4) {
            start_time = (UINT16_T)((p_data[offset + 2] << 8) | p_data[offset + 3]);
        }
        if (length >= 6) {
            end_time = (UINT16_T)((p_data[offset + 4] << 8) | p_data[offset + 5]);
        }
        if (length >= 7) {
            H_hundreds = p_data[offset + 6];
        }
        if (length >= 8) {
            H_tens_ones = p_data[offset + 7];
        }
        if (length >= 9) {
            S = p_data[offset + 8];
        }
        if (length >= 10) {
            V = p_data[offset + 9];
        }
        if (length >= 11) {
            B = p_data[offset + 10];
        }
        if (length >= 12) {
            T = p_data[offset + 11];
        }

        TAL_PR_NOTICE("Random Timing - OnOff: 0x%02x, Week: 0x%02x", on_off, week);
        TAL_PR_NOTICE("Random Timing - Time: %d-%d minutes", start_time, end_time);
        TAL_PR_NOTICE("Random Timing - H: %d%d, S: %d%%, V: %d%%, B: %d%%, T: %d%%", 
                      H_hundreds, H_tens_ones, S, V, B, T);

        /* 验证关键字段 */
        if (on_off != 0x02 && on_off != 0x03) {
            TAL_PR_ERR("Invalid on_off value: 0x%02x (expected 0x02 or 0x03)", on_off);
            TAL_PR_NOTICE("========================================");
            return;
        }
        
        /* 保存计划数据到当前激活计划（用于兼容） */
        s_random_timing_plan.valid = TRUE;
        s_random_timing_plan.version = version;
        s_random_timing_plan.length = length;
        s_random_timing_plan.on_off = on_off;
        s_random_timing_plan.week = week;
        s_random_timing_plan.start_time = start_time;
        s_random_timing_plan.end_time = end_time;
        s_random_timing_plan.H_hundreds = H_hundreds;
        s_random_timing_plan.H_tens_ones = H_tens_ones;
        s_random_timing_plan.S = S;
        s_random_timing_plan.V = V;
        s_random_timing_plan.B = B;
        s_random_timing_plan.T = T;
        
        /* 保存原始数据（完整数据包） */
        UINT_T copy_len = (data_len > sizeof(s_random_timing_plan.raw_data)) ? 
                          sizeof(s_random_timing_plan.raw_data) : data_len;
        memcpy(s_random_timing_plan.raw_data, p_data, copy_len);
        s_random_timing_plan.raw_data_len = copy_len;
        
        /* 保存到多计划数组中（检查是否已存在相同时间段和星期的计划） */
        BOOL_T plan_found = FALSE;
        for (UINT_T i = 0; i < s_random_timing_plan_count; i++) {
            if (s_random_timing_plans[i].start_time == start_time &&
                s_random_timing_plans[i].end_time == end_time &&
                s_random_timing_plans[i].week == week) {
                /* 找到相同时间段和星期的计划，更新它 */
                s_random_timing_plans[i] = s_random_timing_plan;
                plan_found = TRUE;
                TAL_PR_NOTICE("[DP210] Updated existing plan #%d: time=%d-%d week=0x%02x", 
                             i, start_time, end_time, week);
                break;
            }
        }
        
        if (!plan_found) {
            /* 新计划，添加到数组中 */
            if (s_random_timing_plan_count < MAX_RANDOM_TIMING_PLANS) {
                s_random_timing_plans[s_random_timing_plan_count] = s_random_timing_plan;
                s_random_timing_plan_count++;
                TAL_PR_NOTICE("[DP210] Added new plan #%d: time=%d-%d week=0x%02x (total: %d)", 
                             s_random_timing_plan_count - 1, start_time, end_time, week, s_random_timing_plan_count);
            } else {
                /* 数组已满，替换最旧的计划（索引0） */
                TAL_PR_WARN("[DP210] Plan array full, replacing oldest plan");
                for (UINT_T i = 0; i < MAX_RANDOM_TIMING_PLANS - 1; i++) {
                    s_random_timing_plans[i] = s_random_timing_plans[i + 1];
                }
                s_random_timing_plans[MAX_RANDOM_TIMING_PLANS - 1] = s_random_timing_plan;
                TAL_PR_NOTICE("[DP210] Replaced plan, new plan: time=%d-%d week=0x%02x", 
                             start_time, end_time, week);
            }
        }
        
        /* 输出所有保存的计划 */
        TAL_PR_NOTICE("[DP210] All saved plans (%d total):", s_random_timing_plan_count);
        for (UINT_T i = 0; i < s_random_timing_plan_count; i++) {
            TAL_PR_NOTICE("[DP210]   Plan #%d: time=%d-%d(%02d:%02d-%02d:%02d) week=0x%02x on_off=0x%02x", 
                         i, 
                         s_random_timing_plans[i].start_time, s_random_timing_plans[i].end_time,
                         s_random_timing_plans[i].start_time/60, s_random_timing_plans[i].start_time%60,
                         s_random_timing_plans[i].end_time/60, s_random_timing_plans[i].end_time%60,
                         s_random_timing_plans[i].week, s_random_timing_plans[i].on_off);
        }

        TAL_PR_NOTICE("Random Timing - Plan saved: on_off=0x%02x, week=0x%02x, time=%d-%d, B=%d%%, T=%d%%", 
                      on_off, week, start_time, end_time, B, T);
        TAL_PR_NOTICE("Random Timing - Plan parameters: H=%d%d, S=%d%%, V=%d%%, B=%d%%, T=%d%%", 
                      H_hundreds, H_tens_ones, S, V, B, T);

        /* 如果任务开关为开启(0x03)，启动看家模式 */
        if (on_off == 0x03) {
            s_random_timing_active = TRUE;
            /* 如果是"仅一次"模式（week==0），清除之前的执行记录，允许重新执行 */
            if (week == 0) {
                s_once_mode_executed_date = 0;
                TAL_PR_NOTICE("[DP210] 仅一次模式已启用，已清除执行记录");
            } else {
                /* 非"仅一次"模式，输出设置的星期 */
                TAL_PR_NOTICE("[DP210] 重复模式已启用，设置的星期: %s%s%s%s%s%s%s", 
                             (week & 0x01) ? "周日 " : "",
                             (week & 0x02) ? "周一 " : "",
                             (week & 0x04) ? "周二 " : "",
                             (week & 0x08) ? "周三 " : "",
                             (week & 0x10) ? "周四 " : "",
                             (week & 0x20) ? "周五 " : "",
                             (week & 0x40) ? "周六 " : "");
            }
            TAL_PR_NOTICE("[DP210] ENABLED: time=%d-%d(%02d:%02d-%02d:%02d) week=0x%02x B=%d%% T=%d%%", 
                          start_time, end_time, 
                          start_time/60, start_time%60, end_time/60, end_time%60,
                          week, B, T);
            
            /* 获取当前时间用于调试 */
            UINT16_T current_minutes = __get_current_time_minutes();
            BOOL_T week_match_now = __check_week_match(week);
            BOOL_T in_range_now = FALSE;
            if (start_time <= end_time) {
                in_range_now = (current_minutes >= start_time && current_minutes <= end_time);
            } else {
                in_range_now = (current_minutes >= start_time || current_minutes <= end_time);
            }
            TAL_PR_NOTICE("[DP210] Current: time=%d(%02d:%02d) in_range=%d week_match=%d", 
                          current_minutes, current_minutes/60, current_minutes%60, 
                          in_range_now, week_match_now);
            
            /* 如果线程未运行，启动线程 */
            if (s_random_timing_thread == NULL && !s_random_timing_thread_running) {
                THREAD_CFG_T thread_cfg = {
                    .thrdname   = "random_timing",
                    .priority   = THREAD_PRIO_6,
                    .stackDepth = 2048,
                };
                OPERATE_RET rt = tal_thread_create_and_start(&s_random_timing_thread, NULL, NULL, 
                                                             __random_timing_task, NULL, &thread_cfg);
                if (rt != OPRT_OK) {
                    TAL_PR_ERR("[DP210] Thread create FAILED: %d", rt);
                    s_random_timing_active = FALSE;
                } else {
                    s_random_timing_thread_running = TRUE;
                    TAL_PR_NOTICE("[DP210] Thread STARTED");
                }
            } else {
                TAL_PR_NOTICE("[DP210] Thread already running");
            }
        } else if (on_off == 0x02) {
            /* 关闭看家模式（但保留计划数据） */
            s_random_timing_active = FALSE;
            TAL_PR_NOTICE("Random timing disabled (plan kept)");
            
            /* 停止线程 */
            if (s_random_timing_thread != NULL) {
                s_random_timing_thread_running = FALSE;
                tal_system_sleep(100);  /* 等待线程退出 */
                tal_thread_delete(s_random_timing_thread);
                s_random_timing_thread = NULL;
            }
            
            /* 关闭灯光（如果正在看家模式中） */
            if (light_control_get()) {
                light_control_set(FALSE);
                TAL_PR_NOTICE("Random Timing - Light turned OFF after disable");
            }
        }

        /* 上报RAW DP状态 */
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, DPID_RANDOM_TIMING, 
                                                s_random_timing_plan.raw_data, 
                                                s_random_timing_plan.raw_data_len, 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report random_timing: %d", rt);
        } else {
            TAL_PR_DEBUG("Random timing reported successfully");
        }

        /* 上报状态 */
        __report_dp_states(FALSE);
        TAL_PR_NOTICE("[DP210] DONE: on_off=0x%02x time=%d-%d", on_off, start_time, end_time);
        return;
    }

    /* 开灯渐明/关灯渐暗功能 (DPID 101) */
    if (dpid == 101) {
        TAL_PR_NOTICE("=== Parsing Fade In/Out RAW DP ===");
        
        if (data_len < 7) {
            TAL_PR_ERR("Fade in/out RAW DP data too short: %d (expected at least 7 bytes)", data_len);
            TAL_PR_NOTICE("========================================");
            return;
        }
        
        /* 解析数据格式（推测）：
         * [0]: version (1字节，枚举，0x00)
         * [1]: type (1字节，可能表示类型)
         * [2-3]: fade_in_time (2字节，大端序，开灯渐明时间，单位可能是毫秒)
         * [4]: separator (1字节，可能是分隔符)
         * [5-6]: fade_out_time (2字节，大端序，关灯渐暗时间，单位可能是毫秒)
         */
        UINT8_T version = p_data[0];
        UINT8_T type = p_data[1];
        UINT16_T fade_in_time = (UINT16_T)((p_data[2] << 8) | p_data[3]);
        UINT8_T separator = p_data[4];
        UINT16_T fade_out_time = (UINT16_T)((p_data[5] << 8) | p_data[6]);
        
        const UINT16_T MAX_GRADUAL_MS = 10000; /* 10秒上限 */
        BOOL_T fade_in_clamped = FALSE;
        BOOL_T fade_out_clamped = FALSE;
        
        /* 限制最大值：如果超过10秒，限制到10秒 */
        if (fade_in_time > MAX_GRADUAL_MS) {
            TAL_PR_WARN("Fade in time exceeds max (%d ms), clamped to %d ms", fade_in_time, MAX_GRADUAL_MS);
            fade_in_time = MAX_GRADUAL_MS;
            fade_in_clamped = TRUE;
        }
        if (fade_out_time > MAX_GRADUAL_MS) {
            TAL_PR_WARN("Fade out time exceeds max (%d ms), clamped to %d ms", fade_out_time, MAX_GRADUAL_MS);
            fade_out_time = MAX_GRADUAL_MS;
            fade_out_clamped = TRUE;
        }
        
        TAL_PR_NOTICE("Fade In/Out - Version: 0x%02x, Type: 0x%02x", version, type);
        TAL_PR_NOTICE("Fade In/Out - Fade In Time: %d ms, Separator: 0x%02x, Fade Out Time: %d ms", 
                     fade_in_time, separator, fade_out_time);
        TAL_PR_NOTICE("Fade In/Out - Fade In: %d.%d seconds, Fade Out: %d.%d seconds", 
                     fade_in_time / 1000, (fade_in_time % 1000) / 100,
                     fade_out_time / 1000, (fade_out_time % 1000) / 100);
        
        /* 如果值被限制，更新原始数据缓存（用于上报） */
        if (fade_in_clamped || fade_out_clamped) {
            s_fade_in_out_raw[0] = version;
            s_fade_in_out_raw[1] = type;
            s_fade_in_out_raw[2] = (UINT8_T)((fade_in_time >> 8) & 0xFF);
            s_fade_in_out_raw[3] = (UINT8_T)(fade_in_time & 0xFF);
            s_fade_in_out_raw[4] = separator;
            s_fade_in_out_raw[5] = (UINT8_T)((fade_out_time >> 8) & 0xFF);
            s_fade_in_out_raw[6] = (UINT8_T)(fade_out_time & 0xFF);
            s_fade_in_out_raw_len = 7;
            TAL_PR_NOTICE("Fade In/Out - Updated raw data with clamped values");
        } else {
        /* 保存原始数据 */
        UINT_T copy_len = (data_len > sizeof(s_fade_in_out_raw)) ? sizeof(s_fade_in_out_raw) : data_len;
        memcpy(s_fade_in_out_raw, p_data, copy_len);
        s_fade_in_out_raw_len = copy_len;
        }
        
        /* 应用渐明渐暗时间到灯光控制 */
        light_control_update_switch_gradient((UINT32_T)fade_in_time, (UINT32_T)fade_out_time);
        TAL_PR_NOTICE("Fade In/Out - Applied: fade_in=%d ms, fade_out=%d ms", fade_in_time, fade_out_time);
        
        /* 保存到KV存储（断电后恢复） */
        FADE_GRADIENT_STORAGE_T storage = {0};
        storage.magic = FADE_GRADIENT_MAGIC;
        storage.fade_in_time = fade_in_time;
        storage.fade_out_time = fade_out_time;
        OPERATE_RET rt = wd_common_write(KV_KEY_FADE_GRADIENT, (BYTE_T *)&storage, sizeof(storage));
        if (rt == OPRT_OK) {
            TAL_PR_NOTICE("Fade In/Out - Saved to KV: fade_in=%d ms, fade_out=%d ms", fade_in_time, fade_out_time);
        } else {
            TAL_PR_ERR("Fade In/Out - Failed to save to KV: ret=%d", rt);
        }
        
        /* 立即上报RAW DP状态到云端（使用限制后的值） */
        rt = dev_report_dp_raw_sync(NULL, 101, s_fade_in_out_raw, s_fade_in_out_raw_len, 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report fade_in_out: %d", rt);
        } else {
            TAL_PR_NOTICE("Fade In/Out reported successfully to cloud");
        }
        
        TAL_PR_NOTICE("========================================");
        return;
    }
    
    TAL_PR_NOTICE("========================================");
}

VOID upload_device_switch_status(BOOL_T state)
{
    s_switch_state = state ? TRUE : FALSE;
    light_control_set(s_switch_state);
    __report_dp_states(FALSE);
}

/**
 * @brief 从KV加载渐变时间（在初始化时调用）
 */
VOID __fade_gradient_load_from_kv(VOID)
{
    FADE_GRADIENT_STORAGE_T storage = {0};
    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET rt = wd_common_read(KV_KEY_FADE_GRADIENT, &buf, &len);
    
    if (rt == OPRT_OK && buf != NULL && len == sizeof(storage)) {
        memcpy(&storage, buf, sizeof(storage));
        wd_common_free_data(buf);
        
        if (storage.magic == FADE_GRADIENT_MAGIC) {
            /* 应用渐明渐暗时间到灯光控制 */
            light_control_update_switch_gradient((UINT32_T)storage.fade_in_time, 
                                                 (UINT32_T)storage.fade_out_time);
            TAL_PR_NOTICE("[FADE_GRADIENT] Loaded from KV: fade_in=%d ms, fade_out=%d ms", 
                         storage.fade_in_time, storage.fade_out_time);
            
            /* 恢复原始数据缓存（用于上报） */
            s_fade_in_out_raw[0] = 0x00;  /* version */
            s_fade_in_out_raw[1] = 0x00;  /* type */
            s_fade_in_out_raw[2] = (UINT8_T)((storage.fade_in_time >> 8) & 0xFF);
            s_fade_in_out_raw[3] = (UINT8_T)(storage.fade_in_time & 0xFF);
            s_fade_in_out_raw[4] = 0x00;  /* separator */
            s_fade_in_out_raw[5] = (UINT8_T)((storage.fade_out_time >> 8) & 0xFF);
            s_fade_in_out_raw[6] = (UINT8_T)(storage.fade_out_time & 0xFF);
            s_fade_in_out_raw_len = 7;
            
            TAL_PR_NOTICE("[FADE_GRADIENT] Raw data restored for reporting: fade_in=%d ms, fade_out=%d ms", 
                         storage.fade_in_time, storage.fade_out_time);
        } else {
            TAL_PR_NOTICE("[FADE_GRADIENT] Invalid magic in KV, using default values");
        }
    } else {
        if (buf) {
            wd_common_free_data(buf);
        }
        TAL_PR_NOTICE("[FADE_GRADIENT] No KV data found, using default fade times");
    }
}

VOID upload_device_all_status(VOID_T)
{
    s_switch_state = light_control_get();
    s_brightness_value = light_control_get_brightness();
    s_temp_value = light_control_get_color_temp();
    /* work_mode, scene_data, countdown, scene_id, cycle_data 保持当前值，不需要从硬件读取 */
    __report_dp_states(FALSE);
    
    /* 上报渐变时间 RAW DP状态（如果已设置，从KV加载后上报到APP） */
    if (s_fade_in_out_raw_len > 0) {
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, 101, s_fade_in_out_raw, s_fade_in_out_raw_len, 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report fade_in_out: %d", rt);
        } else {
            TAL_PR_DEBUG("Fade in/out reported to APP");
        }
    }
}

/* 获取DP状态的函数 */
BOOL_T dp_get_switch_state(VOID)
{
    return s_switch_state;
}

UINT16_T dp_get_brightness_value(VOID)
{
    return s_brightness_value;
}

UINT16_T dp_get_temp_value(VOID)
{
    return s_temp_value;
}

/**
 * @brief 应用缓存的断电记忆（如果有）
 * @return TRUE=已应用，FALSE=无缓存
 */
BOOL_T dp_apply_cached_power_memory(VOID)
{
    __power_memory_load_cache();

    BOOL_T switch_on = FALSE;
    UINT16_T brightness = 0;
    UINT16_T color_temp = 0;

    if (s_power_memory_cache.valid) {
        switch_on = (s_power_memory_cache.switch_on && s_power_memory_cache.brightness > 0);
        brightness = s_power_memory_cache.brightness;
        color_temp = s_power_memory_cache.color_temp;

        if (s_power_memory_cache.mode == 0x01) {
            REALTIME_LIGHT_STORAGE_T realtime_state = {0};
            if (__load_realtime_light_state(&realtime_state)) {
                switch_on = (realtime_state.switch_on && realtime_state.brightness > 0);
                brightness = realtime_state.brightness;
                color_temp = realtime_state.color_temp;
                TAL_PR_NOTICE("[POWER_MEMORY] Mode 0x01 -> using realtime KV (switch=%d, brightness=%d, temp=%d)",
                              switch_on, brightness, color_temp);
            } else {
                TAL_PR_WARN("[POWER_MEMORY] Mode 0x01 enabled but realtime KV missing, using cached payload");
            }
        }
    } else {
        REALTIME_LIGHT_STORAGE_T realtime_state = {0};
        if (__load_realtime_light_state(&realtime_state)) {
            switch_on = (realtime_state.switch_on && realtime_state.brightness > 0);
            brightness = realtime_state.brightness;
            color_temp = realtime_state.color_temp;
            TAL_PR_NOTICE("[POWER_MEMORY] No DP33 cache, using realtime light KV (switch=%d, brightness=%d, temp=%d)",
                          switch_on, brightness, color_temp);
        } else {
            TAL_PR_NOTICE("[POWER_MEMORY] No cached settings found in KV or realtime store");
            return FALSE;
        }
    }

    if (switch_on) {
        s_switch_state = TRUE;
        s_brightness_value = brightness;
        s_temp_value = color_temp;
        __apply_light_state_with_fade(TRUE, brightness, color_temp);

        TAL_PR_NOTICE("[POWER_MEMORY] Applying cached ON state (mode=0x%02x, brightness=%d, temp=%d)",
                      s_power_memory_cache.mode, brightness, color_temp);
    } else {
        s_switch_state = FALSE;
        s_brightness_value = 0;
        __apply_light_state_with_fade(FALSE, 0, color_temp);
        TAL_PR_NOTICE("[POWER_MEMORY] Cached state is OFF, keeping light OFF");
    }

    return TRUE;
}

/**
 * @brief 等待断电记忆DP 33下发（用于上电恢复）
 * @param timeout_ms 超时时间（毫秒）
 * @return TRUE=已收到DP 33且断电记忆已启用，FALSE=超时或未启用
 */
BOOL_T dp_wait_power_memory(UINT32_T timeout_ms)
{
    __power_memory_load_cache();
    UINT32_T start_ms = tal_system_get_millisecond();
    UINT32_T elapsed_ms = 0;
    UINT32_T check_count = 0;
    
    /* 限制最大等待时间，避免看门狗复位 */
    if (timeout_ms > 10000) {
        timeout_ms = 10000;  /* 最多等待10秒 */
    }
    
    while (elapsed_ms < timeout_ms) {
        /* 检查是否已收到DP 33 */
        if (s_power_memory_synced_runtime) {
            /* 已收到DP 33，检查是否启用 */
            if (s_power_memory_enabled) {
                TAL_PR_NOTICE("[POWER_MEMORY] Power memory received and enabled");
                return TRUE;
            } else {
                TAL_PR_NOTICE("[POWER_MEMORY] Power memory received but disabled (mode=0x00)");
                return FALSE;
            }
        }
        
        /* 计算已过时间，处理溢出情况 */
        UINT32_T now_ms = tal_system_get_millisecond();
        if (now_ms >= start_ms) {
            elapsed_ms = now_ms - start_ms;
        } else {
            /* 发生溢出，重新计算 */
            elapsed_ms = (UINT32_T)(0xFFFFFFFF - start_ms) + now_ms;
            start_ms = now_ms;  /* 更新起始时间 */
        }
        
        /* 每1秒输出一次日志，避免长时间无输出 */
        check_count++;
        if (check_count % 10 == 0) {
            TAL_PR_DEBUG("[POWER_MEMORY] Waiting for DP 33... (elapsed=%lu ms)", (unsigned long)elapsed_ms);
        }
        
        tal_system_sleep(100);  /* 等待100ms后再次检查 */
    }
    
    TAL_PR_NOTICE("[POWER_MEMORY] Timeout waiting for power memory DP (timeout=%lu ms)", (unsigned long)timeout_ms);
    return FALSE;
}

/**
 * @brief 检查断电记忆是否已启用
 * @return TRUE=已启用，FALSE=未启用
 */
BOOL_T dp_is_power_memory_enabled(VOID)
{
    __power_memory_load_cache();
    return s_power_memory_enabled;
}

/**
 * @brief 检查是否已收到断电记忆DP 33
 * @return TRUE=已收到，FALSE=未收到
 */
BOOL_T dp_is_power_memory_received(VOID)
{
    __power_memory_load_cache();
    return s_power_memory_received;
}

/**
 * @brief 加载通电勿扰状态从KV
 */
STATIC VOID __dnd_state_load(VOID)
{
    if (s_dnd_state_loaded) {
        return;
    }

    BYTE_T *buf = NULL;
    UINT_T len = 0;
    OPERATE_RET rt = wd_common_read(KV_KEY_DND_STATE, &buf, &len);
    if (rt == OPRT_OK && buf != NULL && len == sizeof(DND_STATE_STORAGE_T)) {
        DND_STATE_STORAGE_T storage = {0};
        memcpy(&storage, buf, sizeof(storage));
        if (storage.magic == DND_STATE_MAGIC) {
            s_do_not_disturb = (storage.enabled != 0) ? TRUE : FALSE;
            TAL_PR_NOTICE("[DO_NOT_DISTURB] Loaded state from KV: %s", s_do_not_disturb ? "ENABLED" : "DISABLED");
        } else {
            s_do_not_disturb = FALSE;
        }
    } else {
        s_do_not_disturb = FALSE;
    }

    if (buf) {
        wd_common_free_data(buf);
    }

    s_dnd_state_loaded = TRUE;
}

/**
 * @brief 保存通电勿扰状态到KV
 */
STATIC VOID __dnd_state_save(VOID)
{
    DND_STATE_STORAGE_T storage = {0};
    storage.magic = DND_STATE_MAGIC;
    storage.enabled = s_do_not_disturb ? 1 : 0;
    
    wd_common_write(KV_KEY_DND_STATE, (BYTE_T *)&storage, sizeof(storage));
    TAL_PR_DEBUG("[DO_NOT_DISTURB] Saved state to KV: %s", s_do_not_disturb ? "ENABLED" : "DISABLED");
}

/**
 * @brief 延迟保存通电勿扰状态（如果标记需要保存）
 * @note 在后台任务中调用，避免在DP处理的关键路径上立即写入KV导致看门狗复位
 */
VOID dp_dnd_state_delayed_save(VOID)
{
    if (s_dnd_state_need_save) {
        __dnd_state_save();
        s_dnd_state_need_save = FALSE;
        TAL_PR_NOTICE("[DO_NOT_DISTURB] State saved to KV: %s", s_do_not_disturb ? "ENABLED" : "DISABLED");
    }
}

/**
 * @brief 获取通电勿扰状态
 * @return TRUE=已开启，FALSE=未开启
 */
BOOL_T dp_get_do_not_disturb_state(VOID)
{
    __dnd_state_load();
    return s_do_not_disturb;
}

/**
 * @brief 检查通电勿扰的上电次数（内部函数）
 * @return TRUE=需要阻止灯光恢复（未达到要求的上电次数），FALSE=允许灯光恢复
 * @note 使用断电复位的KV（power_cycle）来存储上电次数，与断电复位功能共用
 */
STATIC BOOL_T __check_do_not_disturb_power_cycle(VOID)
{
    /* 如果通电勿扰未开启，直接允许恢复 */
    if (!s_do_not_disturb) {
        return FALSE;
    }

    /* 读取断电计数（与断电复位共用同一个KV） */
    UINT32_T count = __dnd_get_power_cycle_count();
    
    /* 本次上电时，如果计数小于要求值，说明还未达到要求 */
    /* 注意：断电复位模块会在每次上电时自动计数+1，这里只需要读取即可 */
    TAL_PR_NOTICE("[DO_NOT_DISTURB] Power cycle count = %d / %d (using shared KV with power cycle reset)",
                  count, POWER_CYCLE_REQUIRED);

    if (count < POWER_CYCLE_REQUIRED) {
        TAL_PR_NOTICE("[DO_NOT_DISTURB] Requirement not met (count < %d), blocking light restore", POWER_CYCLE_REQUIRED);
        return TRUE;  /* 需要阻止灯光恢复 */
    }

    TAL_PR_NOTICE("[DO_NOT_DISTURB] Requirement satisfied (count >= %d), allowing restore", POWER_CYCLE_REQUIRED);
    /* 达到要求后，不需要清零计数，因为断电复位模块会管理计数 */
    return FALSE;  /* 允许灯光恢复 */
}

/**
 * @brief 检查通电勿扰的上电次数（供外部调用）
 * @return TRUE=需要延迟初始化（未达到要求的上电次数），FALSE=可以正常初始化
 */
BOOL_T dp_check_do_not_disturb_power_cycle(VOID)
{
    return __check_do_not_disturb_power_cycle();
}

VOID_T respone_device_all_status(VOID_T)
{
    s_switch_state = light_control_get();
    s_brightness_value = light_control_get_brightness();
    s_temp_value = light_control_get_color_temp();
    /* work_mode, scene_data, countdown, scene_id, cycle_data 保持当前值，不需要从硬件读取 */
    __report_dp_states(TRUE);
    /* 同时上报sleep_mode RAW DP状态 */
    __report_all_sleep_plans();
    /* 上报断电记忆 RAW DP状态（如果已启用，从当前DP状态构造） */
    if (s_power_memory_enabled) {
        UINT8_T report_data[12] = {0};
        report_data[0] = 0x00;  /* version */
        report_data[1] = 0x02;  /* mode=0x02 (用户定制) */
        report_data[2] = 0x00;  /* H=0 */
        report_data[3] = 0x00;
        report_data[4] = 0x03;  /* S=1000 */
        report_data[5] = 0xE8;
        report_data[6] = 0x03;  /* V=1000 */
        report_data[7] = 0xE8;
        report_data[8] = (s_brightness_value >> 8) & 0xFF;  /* B */
        report_data[9] = s_brightness_value & 0xFF;
        report_data[10] = (s_temp_value >> 8) & 0xFF;  /* T */
        report_data[11] = s_temp_value & 0xFF;
        
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, DPID_POWER_MEMORY, report_data, sizeof(report_data), 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report power_memory: %d", rt);
        } else {
            TAL_PR_DEBUG("Power memory reported");
        }
    }
    /* 上报渐变时间 RAW DP状态（如果已设置） */
    if (s_fade_in_out_raw_len > 0) {
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, 101, s_fade_in_out_raw, s_fade_in_out_raw_len, 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report fade_in_out: %d", rt);
        } else {
            TAL_PR_DEBUG("Fade in/out reported");
        }
    }
    
    /* 上报灯光看家 RAW DP状态（上报所有保存的计划） */
    if (s_random_timing_plan_count > 0) {
        /* 上报当前激活的计划（用于兼容） */
        if (s_random_timing_plan.raw_data_len > 0) {
            OPERATE_RET rt = dev_report_dp_raw_sync(NULL, DPID_RANDOM_TIMING, 
                                                    s_random_timing_plan.raw_data, 
                                                    s_random_timing_plan.raw_data_len, 5);
            if (rt != OPRT_OK) {
                TAL_PR_ERR("Failed to report random_timing: %d", rt);
            } else {
                TAL_PR_DEBUG("Random timing reported (%d plans saved)", s_random_timing_plan_count);
            }
        }
    } else if (s_random_timing_plan.raw_data_len > 0) {
        /* 兼容旧代码：如果没有多计划，上报单个计划 */
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, DPID_RANDOM_TIMING, 
                                                s_random_timing_plan.raw_data, 
                                                s_random_timing_plan.raw_data_len, 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report random_timing: %d", rt);
        } else {
            TAL_PR_DEBUG("Random timing reported (single plan)");
        }
    }
    
    /* 上报渐明渐暗 RAW DP状态 */
    if (s_fade_in_out_raw_len > 0) {
        OPERATE_RET rt = dev_report_dp_raw_sync(NULL, 101, s_fade_in_out_raw, s_fade_in_out_raw_len, 5);
        if (rt != OPRT_OK) {
            TAL_PR_ERR("Failed to report fade_in_out: %d", rt);
        } else {
            TAL_PR_DEBUG("Fade in/out reported");
        }
    }
}

/**
 * @brief 检查当前星期是否符合计划设置
 * @param week 周数据（位掩码：bit0=周日，bit1=周一，...，bit6=周六）
 * @return TRUE=符合，FALSE=不符合
 */
STATIC BOOL_T __check_week_match(UINT8_T week)
{
    if (week == 0) {
        /* 全为0表示"仅一次"模式，只生效一次（同一天内） */
        /* 获取当前日期（格式：YYYYMMDD） */
        TIME_T time_sec = tal_time_get_posix();
        if (time_sec < 1577836800) {  /* 2020-01-01 00:00:00 UTC */
            TAL_PR_ERR("[DP210] Week check (once mode): time invalid: time_sec=%lu", (unsigned long)time_sec);
            return FALSE;
        }
        
        POSIX_TM_S local_tm = {0};
        OPERATE_RET ret = tal_time_get_local_time_custom(time_sec, &local_tm);
        if (ret != OPRT_OK) {
            struct tm *time_info = localtime((time_t *)&time_sec);
            if (time_info == NULL) {
                TAL_PR_ERR("[DP210] Week check (once mode): time convert FAILED");
                return FALSE;
            }
            local_tm.tm_year = time_info->tm_year;
            local_tm.tm_mon = time_info->tm_mon;
            local_tm.tm_mday = time_info->tm_mday;
        }
        
        /* 计算日期（YYYYMMDD） */
        UINT32_T current_date = (UINT32_T)((local_tm.tm_year + 1900) * 10000 + 
                                          (local_tm.tm_mon + 1) * 100 + 
                                          local_tm.tm_mday);
        
        /* 如果已经执行过（同一天），返回FALSE */
        if (s_once_mode_executed_date == current_date) {
            TAL_PR_NOTICE("[DP210] Week check (仅一次模式): 今天已执行过 (date=%04d-%02d-%02d)", 
                        current_date / 10000, (current_date % 10000) / 100, current_date % 100);
            return FALSE;
        }
        
        /* 仅一次模式：检查通过，但不在这里标记（在任务真正开始执行时标记） */
        TAL_PR_NOTICE("[DP210] Week check (仅一次模式): 今天可以执行 (date=%04d-%02d-%02d)", 
                     current_date / 10000, (current_date % 10000) / 100, current_date % 100);
        return TRUE;
    }

    /* 使用 tal_time_get_posix() 获取系统时间（与 __get_current_time_minutes 一致） */
    TIME_T time_sec = tal_time_get_posix();
    
    /* 检查时间是否有效 */
    if (time_sec < 1577836800) {  /* 2020-01-01 00:00:00 UTC */
        TAL_PR_ERR("[DP210] Week check: time invalid: time_sec=%lu", (unsigned long)time_sec);
        return FALSE;
    }

    /* 使用 tal_time_get_local_time_custom 获取本地时间（包含时区） */
    POSIX_TM_S local_tm = {0};
    OPERATE_RET ret = tal_time_get_local_time_custom(time_sec, &local_tm);
    if (ret != OPRT_OK) {
        /* 如果获取本地时间失败，尝试使用 localtime */
        struct tm *time_info = localtime((time_t *)&time_sec);
        if (time_info == NULL) {
            TAL_PR_ERR("[DP210] Week check: time convert FAILED: time_sec=%lu", (unsigned long)time_sec);
            return FALSE;
        }
        local_tm.tm_wday = time_info->tm_wday;
    }

    /* 获取星期（0=周日，1=周一，...，6=周六） */
    UINT8_T current_weekday = (UINT8_T)local_tm.tm_wday;
    
    /* 边界检查：确保 weekday 在有效范围内（0-6） */
    if (current_weekday > 6) {
        TAL_PR_ERR("[DP210] Week check: Invalid weekday=%d (should be 0-6)", current_weekday);
        return FALSE;
    }
    
    /* 检查对应位是否置1 */
    /* week位掩码：bit0=周日(0x01), bit1=周一(0x02), bit2=周二(0x04), bit3=周三(0x08), 
     *             bit4=周四(0x10), bit5=周五(0x20), bit6=周六(0x40) */
    UINT8_T week_bit = (UINT8_T)(1 << current_weekday);
    BOOL_T match = (week & week_bit) ? TRUE : FALSE;
    
    /* 星期名称映射 */
    const CHAR_T *weekday_names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    const CHAR_T *weekday_name = (current_weekday < 7) ? weekday_names[current_weekday] : "未知";
    
    TAL_PR_NOTICE("[DP210] Week check: 今天=%s(weekday=%d) week=0x%02x bit=0x%02x match=%d", 
                 weekday_name, current_weekday, week, week_bit, match);
    
    /* 输出week中设置的星期（用于调试） */
    if (week != 0) {
        TAL_PR_NOTICE("[DP210] Week check: 计划设置的星期: %s%s%s%s%s%s%s", 
                     (week & 0x01) ? "周日 " : "",
                     (week & 0x02) ? "周一 " : "",
                     (week & 0x04) ? "周二 " : "",
                     (week & 0x08) ? "周三 " : "",
                     (week & 0x10) ? "周四 " : "",
                     (week & 0x20) ? "周五 " : "",
                     (week & 0x40) ? "周六 " : "");
    }
    
    return match;
}

/**
 * @brief 获取当前时间（分钟数，0-1439）
 * @return 当前时间的分钟数
 */
STATIC UINT16_T __get_current_time_minutes(VOID)
{
    /* 使用 tal_time_get_posix() 获取系统时间（UTC时间戳，秒数） */
    /* 这是定时开关代码中使用的方法，时间获取是准的 */
    TIME_T time_sec = tal_time_get_posix();
    
    /* 检查时间是否有效（应该是一个合理的时间戳，例如2020年之后） */
    if (time_sec >= 1577836800) {  /* 2020-01-01 00:00:00 UTC */
        /* 使用 tal_time_get_local_time_custom 获取本地时间（包含时区） */
        POSIX_TM_S local_tm = {0};
        OPERATE_RET ret = tal_time_get_local_time_custom(time_sec, &local_tm);
        if (ret == OPRT_OK) {
            UINT16_T minutes = (UINT16_T)(local_tm.tm_hour * 60 + local_tm.tm_min);
            /* 只在第一次成功时输出，避免日志过多 */
            static BOOL_T first_success = TRUE;
            if (first_success) {
                TAL_PR_NOTICE("[DP210] Time SYNCED! time_sec=%lu -> %02d:%02d (%d min) [LOCAL]", 
                             (unsigned long)time_sec, local_tm.tm_hour, local_tm.tm_min, minutes);
                first_success = FALSE;
            }
            return minutes;
        } else {
            /* 如果获取本地时间失败，尝试使用 localtime（可能时区未设置） */
            struct tm *time_info = localtime((time_t *)&time_sec);
            if (time_info != NULL) {
                UINT16_T minutes = (UINT16_T)(time_info->tm_hour * 60 + time_info->tm_min);
                TAL_PR_WARN("[DP210] Using UTC time (timezone not set?): %02d:%02d (%d min)", 
                           time_info->tm_hour, time_info->tm_min, minutes);
                return minutes;
            } else {
                TAL_PR_ERR("[DP210] Time convert FAILED: time_sec=%lu", (unsigned long)time_sec);
            }
        }
    } else {
        TAL_PR_ERR("[DP210] Time invalid: time_sec=%lu (too old, not synced?)", (unsigned long)time_sec);
    }
    
    /* 如果 tal_time_get_posix 失败，尝试使用 tkl_rtc_time_get 作为备选 */
    TIME_T rtc_time_sec = 0;
    OPERATE_RET ret = tkl_rtc_time_get(&rtc_time_sec);
    if (ret == OPRT_OK && rtc_time_sec >= 1577836800) {
        POSIX_TM_S local_tm = {0};
        if (tal_time_get_local_time_custom(rtc_time_sec, &local_tm) == OPRT_OK) {
            UINT16_T minutes = (UINT16_T)(local_tm.tm_hour * 60 + local_tm.tm_min);
            TAL_PR_NOTICE("[DP210] Time from RTC: time_sec=%lu -> %02d:%02d (%d min) [LOCAL]", 
                         (unsigned long)rtc_time_sec, local_tm.tm_hour, local_tm.tm_min, minutes);
            return minutes;
        }
    }
    
    /* 所有方法都失败 */
    TAL_PR_ERR("[DP210] All time get methods FAILED");
    /* 返回一个明显无效的值，避免误判为有效时间 */
    return 0xFFFF;  /* 返回65535，表示时间无效 */
}

/**
 * @brief 随机亮灭任务线程
 * 在设定的时间范围内，随机控制灯光开关，模拟有人在家的场景
 */
STATIC VOID __random_timing_task(VOID_T *arg)
{
    (VOID)arg;

    TAL_PR_NOTICE("[DP210] ========================================");
    TAL_PR_NOTICE("[DP210] TASK STARTED");
    TAL_PR_NOTICE("[DP210] Plan: time=%d-%d(%02d:%02d-%02d:%02d) week=0x%02x B=%d%% T=%d%%", 
                  s_random_timing_plan.start_time, s_random_timing_plan.end_time,
                  s_random_timing_plan.start_time/60, s_random_timing_plan.start_time%60,
                  s_random_timing_plan.end_time/60, s_random_timing_plan.end_time%60,
                  s_random_timing_plan.week, s_random_timing_plan.B, s_random_timing_plan.T);
    TAL_PR_NOTICE("[DP210] Waiting for time sync...");
    TAL_PR_NOTICE("[DP210] ========================================");

    /* 随机亮灭参数（调整为更频繁的开关） */
    const UINT32_T MIN_RANDOM_INTERVAL_MS = 10000;   /* 最小随机间隔：10秒 */
    const UINT32_T MAX_RANDOM_INTERVAL_MS = 60000;   /* 最大随机间隔：1分钟 */
    const UINT32_T MIN_ON_DURATION_MS = 20000;       /* 最小亮灯时长：20秒 */
    const UINT32_T MAX_ON_DURATION_MS = 120000;      /* 最大亮灯时长：2分钟 */
    const UINT32_T MIN_OFF_DURATION_MS = 15000;      /* 最小灭灯时长：15秒 */
    const UINT32_T MAX_OFF_DURATION_MS = 60000;      /* 最大灭灯时长：1分钟 */

    BOOL_T light_state = FALSE;
    UINT32_T next_action_time = 0;
    UINT32_T action_duration = 0;
    UINT32_T last_state_change_time = 0;  /* 上次状态改变的时间（用于计算持续时间） */
    BOOL_T last_in_time_range = FALSE;
    BOOL_T last_week_match = FALSE;
    BOOL_T intro_done = FALSE;  /* 是否已经完成初始开灯 */
    BOOL_T last_actual_light_state = FALSE;  /* 上次检查的实际灯光状态 */
    UINT32_T last_state_check_time = 0;  /* 上次检查状态的时间 */
    UINT32_T external_control_detected_time = 0;  /* 检测到外部控制的时间 */
    BOOL_T initial_state_synced = FALSE;  /* 是否已经同步初始状态 */
    #define EXTERNAL_CONTROL_PAUSE_MS 120000  /* 检测到外部控制后，暂停看家灯光120秒（2分钟） */
    #define STATE_CHECK_INTERVAL_MS 5000  /* 每5秒检查一次实际灯光状态 */

    while (s_random_timing_thread_running) {
        if (!s_random_timing_active || !s_random_timing_plan.valid) {
            TAL_PR_DEBUG("[DP210] Task wait: active=%d valid=%d", 
                         s_random_timing_active, s_random_timing_plan.valid);
            tal_system_sleep(1000);
            continue;
        }

        /* 如果网络指示器活跃，不执行看家模式 */
        if (light_control_network_indicator_active()) {
            TAL_PR_DEBUG("[DP210] Task blocked: network indicator active");
            tal_system_sleep(1000);
            continue;
        }

        UINT32_T now = tal_system_get_millisecond();
        
        /* 检查用户是否手动控制了灯光，如果是，暂停看家灯光一段时间 */
        if (s_user_manual_control_time > 0) {
            UINT32_T elapsed = now - s_user_manual_control_time;
            if (elapsed < USER_MANUAL_CONTROL_PAUSE_MS) {
                /* 还在暂停期内，不执行看家灯光控制 */
                static UINT32_T last_pause_log = 0;
                if (last_pause_log == 0 || now - last_pause_log > 10000) {
                    TAL_PR_DEBUG("[DP210] Task paused: user manual control (remaining %lu seconds)", 
                                (USER_MANUAL_CONTROL_PAUSE_MS - elapsed) / 1000);
                    last_pause_log = now;
                }
                tal_system_sleep(1000);
                continue;
            } else {
                /* 暂停期已过，清除手动控制标记，恢复看家灯光控制 */
                TAL_PR_NOTICE("[DP210] User manual control pause expired, resuming random timing");
                s_user_manual_control_time = 0;
                /* 重置状态，让看家灯光重新开始 */
                light_state = FALSE;
                intro_done = FALSE;
                next_action_time = 0;
                action_duration = 0;
                last_actual_light_state = FALSE;
                external_control_detected_time = 0;
            }
        }
        
        /* 检查是否检测到外部控制（定时开关等），如果是，暂停看家灯光一段时间 */
        if (external_control_detected_time > 0) {
            UINT32_T elapsed = now - external_control_detected_time;
            if (elapsed < EXTERNAL_CONTROL_PAUSE_MS) {
                /* 还在暂停期内，不执行看家灯光控制 */
                static UINT32_T last_external_pause_log = 0;
                if (last_external_pause_log == 0 || now - last_external_pause_log > 10000) {
                    TAL_PR_DEBUG("[DP210] Task paused: external control detected (timer switch?) (remaining %lu seconds)", 
                                (EXTERNAL_CONTROL_PAUSE_MS - elapsed) / 1000);
                    last_external_pause_log = now;
                }
                tal_system_sleep(1000);
                continue;
            } else {
                /* 暂停期已过，清除外部控制标记，恢复看家灯光控制 */
                TAL_PR_NOTICE("[DP210] External control pause expired, resuming random timing");
                external_control_detected_time = 0;
                /* 重置状态，让看家灯光重新开始 */
                light_state = FALSE;
                intro_done = FALSE;
                next_action_time = 0;
                action_duration = 0;
                last_actual_light_state = FALSE;
            }
        }
        
        /* 首次启动时，同步实际灯光状态，避免误判外部控制 */
        if (!initial_state_synced) {
            BOOL_T actual_light_state = light_control_get();
            light_state = actual_light_state;
            last_actual_light_state = actual_light_state;
            last_state_check_time = now;
            initial_state_synced = TRUE;
            TAL_PR_DEBUG("[DP210] Initial state synced: light_state=%d", light_state);
        }
        
        /* 定期检查实际灯光状态，检测是否被定时开关等外部任务控制 */
        if (last_state_check_time == 0 || now - last_state_check_time >= STATE_CHECK_INTERVAL_MS) {
            BOOL_T actual_light_state = light_control_get();
            last_state_check_time = now;
            
            /* 如果实际灯光状态与期望状态不一致，且不是刚设置的状态，可能是外部控制 */
            if (actual_light_state != light_state && 
                (now - next_action_time > 2000)) {  /* 至少2秒前设置的，避免误判 */
                /* 检测到外部控制（可能是定时开关） */
                if (external_control_detected_time == 0) {
                    external_control_detected_time = now;
                    TAL_PR_NOTICE("[DP210] External control detected! Expected=%d, Actual=%d (timer switch?), pausing for %d seconds", 
                                 light_state, actual_light_state, EXTERNAL_CONTROL_PAUSE_MS / 1000);
                    /* 同步期望状态到实际状态 */
                    light_state = actual_light_state;
                    last_actual_light_state = actual_light_state;
                    tal_system_sleep(1000);
                    continue;
                }
            }
            last_actual_light_state = actual_light_state;
        }
        
        UINT16_T current_minutes = __get_current_time_minutes();
        UINT16_T start_time = s_random_timing_plan.start_time;
        UINT16_T end_time = s_random_timing_plan.end_time;

        /* 检查是否在时间范围内 */
        BOOL_T in_time_range = FALSE;
        /* 如果时间无效（0xFFFF），不判断时间段 */
        if (current_minutes == 0xFFFF) {
            in_time_range = FALSE;
            TAL_PR_WARN("[DP210] Time invalid (0xFFFF), skipping time range check");
        } else {
            if (start_time <= end_time) {
                /* 正常时间范围（例如 18:00-22:00） */
                in_time_range = (current_minutes >= start_time && current_minutes <= end_time);
            } else {
                /* 跨天时间范围（例如 22:00-06:00） */
                in_time_range = (current_minutes >= start_time || current_minutes <= end_time);
            }
        }

        /* 检查星期是否符合 */
        BOOL_T week_match = __check_week_match(s_random_timing_plan.week);
        /* 如果时间无效，星期匹配也设为FALSE */
        if (current_minutes == 0xFFFF) {
            week_match = FALSE;
        }

        /* 检测刚进入时间段 */
        BOOL_T just_entered = FALSE;
        if ((in_time_range && week_match) && !(last_in_time_range && last_week_match)) {
            just_entered = TRUE;
            intro_done = FALSE;  /* 重置初始状态 */
            TAL_PR_NOTICE("[DP210] >>> ENTERED time range: now=%d(%02d:%02d) start=%d(%02d:%02d) end=%d(%02d:%02d) week=0x%02x", 
                          current_minutes, current_minutes/60, current_minutes%60,
                          start_time, start_time/60, start_time%60,
                          end_time, end_time/60, end_time%60,
                          s_random_timing_plan.week);
        }
        
        /* 检测离开时间段 */
        if (!(in_time_range && week_match) && (last_in_time_range && last_week_match)) {
            TAL_PR_NOTICE("[DP210] <<< LEFT time range: now=%d(%02d:%02d)", 
                          current_minutes, current_minutes/60, current_minutes%60);
            intro_done = FALSE;  /* 重置初始状态 */
            
            /* 仅一次模式：在离开时间段时标记为已执行（表示本次执行完成），并自动关闭任务 */
            if (s_random_timing_plan.week == 0) {
                TIME_T time_sec = tal_time_get_posix();
                if (time_sec >= 1577836800) {  /* 时间有效 */
                    POSIX_TM_S local_tm = {0};
                    OPERATE_RET ret = tal_time_get_local_time_custom(time_sec, &local_tm);
                    if (ret != OPRT_OK) {
                        struct tm *time_info = localtime((time_t *)&time_sec);
                        if (time_info != NULL) {
                            local_tm.tm_year = time_info->tm_year;
                            local_tm.tm_mon = time_info->tm_mon;
                            local_tm.tm_mday = time_info->tm_mday;
                        }
                    }
                    UINT32_T current_date = (UINT32_T)((local_tm.tm_year + 1900) * 10000 + 
                                                      (local_tm.tm_mon + 1) * 100 + 
                                                      local_tm.tm_mday);
                    s_once_mode_executed_date = current_date;
                    TAL_PR_NOTICE("[DP210] 仅一次模式: 执行完成，标记为已执行 (date=%04d-%02d-%02d)", 
                                 current_date / 10000, (current_date % 10000) / 100, current_date % 100);
                    
                    /* 自动关闭看家模式（仅一次模式执行完成后关闭） */
                    s_random_timing_active = FALSE;
                    TAL_PR_NOTICE("[DP210] 仅一次模式: 任务执行完成，自动关闭看家模式");
                    
                    /* 关闭灯光（如果正在看家模式中） */
                    if (light_control_get()) {
                        light_control_set(FALSE);
                        TAL_PR_NOTICE("[DP210] 仅一次模式: 关闭灯光");
                    }
                }
            }
        }
        
        /* 定期输出状态（每10秒一次，便于调试） */
        static UINT32_T last_status_log = 0;
        static UINT32_T time_sync_retry_count = 0;
        if (last_status_log == 0 || now - last_status_log > 10000) {
            if (current_minutes == 0xFFFF) {
                /* 时间无效，输出警告并增加重试计数 */
                time_sync_retry_count++;
                TAL_PR_NOTICE("[DP210] Loop: TIME NOT SYNCED! (retry #%lu) Waiting for time sync... start=%d(%02d:%02d) end=%d(%02d:%02d) active=%d", 
                              (unsigned long)time_sync_retry_count,
                              start_time, start_time/60, start_time%60,
                              end_time, end_time/60, end_time%60,
                              s_random_timing_active);
            } else {
                /* 时间有效，输出正常状态并重置重试计数 */
                if (time_sync_retry_count > 0) {
                    TAL_PR_NOTICE("[DP210] Time sync SUCCESS after %lu retries!", (unsigned long)time_sync_retry_count);
                    time_sync_retry_count = 0;
                }
                TAL_PR_NOTICE("[DP210] Loop: now=%d(%02d:%02d) start=%d(%02d:%02d) end=%d(%02d:%02d) in_range=%d week=0x%02x match=%d light=%d active=%d", 
                              current_minutes, current_minutes/60, current_minutes%60,
                              start_time, start_time/60, start_time%60,
                              end_time, end_time/60, end_time%60,
                              in_time_range, s_random_timing_plan.week, week_match, light_state, s_random_timing_active);
            }
            last_status_log = now;
        }
        
        last_in_time_range = in_time_range;
        last_week_match = week_match;

        if (in_time_range && week_match) {
            /* 刚进入时间段时，先开启灯光 */
            if (just_entered || !intro_done) {
                /* 进入看家时间段时，先按照计划开启灯光 */
                UINT16_T brightness = (UINT16_T)s_random_timing_plan.B * 10;
                
                /* 如果亮度为0，使用默认亮度（50%） */
                if (brightness == 0) {
                    brightness = BRIGHT_VALUE_MAX / 2;  /* 默认50%亮度 */
                    TAL_PR_WARN("[DP210] Brightness is 0, using default 50%%");
                } else if (brightness < BRIGHT_VALUE_MIN && brightness > 0) {
                    brightness = BRIGHT_VALUE_MIN;
                } else if (brightness > BRIGHT_VALUE_MAX) {
                    brightness = BRIGHT_VALUE_MAX;
                }

                UINT16_T temp = (UINT16_T)s_random_timing_plan.T * 10;
                /* 如果色温为0，不设置色温（保持当前色温） */
                if (temp > TEMP_VALUE_MAX) {
                    temp = TEMP_VALUE_MAX;
                }
                /* 注意：如果 temp == 0，不调用 light_control_set_color_temp，保持当前色温 */

                /* 先开启灯光 */
                light_state = TRUE;
                light_control_set(TRUE);
                light_control_set_brightness(brightness);
                /* 只有当色温不为0时才设置色温 */
                if (temp > 0) {
                    light_control_set_color_temp(temp);
                    TAL_PR_NOTICE("[DP210] Light ON at start: B=%d%%(raw=%d->%d) T=%d%%(raw=%d->%d)", 
                                  s_random_timing_plan.B, s_random_timing_plan.B, brightness,
                                  s_random_timing_plan.T, s_random_timing_plan.T, temp);
                } else {
                    TAL_PR_NOTICE("[DP210] Light ON at start: B=%d%%(raw=%d->%d) T=0(keep current)", 
                                  s_random_timing_plan.B, s_random_timing_plan.B, brightness);
                }
                last_actual_light_state = TRUE;  /* 更新实际状态记录 */
                last_state_change_time = now;  /* 记录状态改变时间 */

                /* 设置初始随机开关时间：先保持开启一段时间（10秒-30秒随机），然后开始随机亮灭 */
                UINT32_T initial_on_duration = 10000 + (rand() % 20000); /* 10秒到30秒之间 */
                action_duration = now + initial_on_duration;
                next_action_time = now + 2000;  /* 设置2秒后检查，避免误判外部控制 */
                intro_done = TRUE;
                tal_system_sleep(1000);
                continue;
            }

            /* 在有效时间内，执行随机亮灭 */
            /* 使用 action_duration 作为主要判断，确保在达到时长时执行操作 */
            if (now >= action_duration) {
                if (light_state) {
                    /* 当前是亮灯状态，随机决定是否关灯 */
                    INT32_T rand_val = rand() % 100;
                    if (rand_val < 30) {  /* 30%概率关灯 */
                        UINT32_T on_duration = (last_state_change_time > 0) ? (now - last_state_change_time) / 1000 : 0;
                        light_state = FALSE;
                        light_control_set(FALSE);
                        last_actual_light_state = FALSE;  /* 更新实际状态记录 */
                        next_action_time = now + 2000;  /* 记录设置时间，避免误判 */
                        last_state_change_time = now;  /* 记录状态改变时间 */
                        TAL_PR_NOTICE("[DP210] Random: Light OFF (was on for %lu seconds)", on_duration);
                        
                        /* 随机生成灭灯时长 */
                        action_duration = now + MIN_OFF_DURATION_MS + 
                                        (rand() % (MAX_OFF_DURATION_MS - MIN_OFF_DURATION_MS));
                        TAL_PR_NOTICE("[DP210] Random: Will check again in %lu seconds (off duration)", 
                                     (action_duration - now) / 1000);
                    } else {
                        /* 继续亮灯，延长亮灯时长 */
                        action_duration = now + MIN_ON_DURATION_MS + 
                                        (rand() % (MAX_ON_DURATION_MS - MIN_ON_DURATION_MS));
                        TAL_PR_DEBUG("[DP210] Random: Continue ON, next check in %lu seconds", 
                                    (action_duration - now) / 1000);
                    }
                } else {
                    /* 当前是灭灯状态，随机决定是否开灯 */
                    INT32_T rand_val = rand() % 100;
                    if (rand_val < 70) {  /* 70%概率开灯 */
                        UINT32_T off_duration = (last_state_change_time > 0) ? (now - last_state_change_time) / 1000 : 0;
                        light_state = TRUE;
                        
                        /* 应用配置的灯光参数 */
                        UINT16_T brightness = (UINT16_T)s_random_timing_plan.B * 10;  /* 0-100 -> 0-1000 */
                        if (brightness == 0) {
                            brightness = BRIGHT_VALUE_MAX / 2;  /* 默认50%亮度 */
                            TAL_PR_WARN("[DP210] Brightness is 0, using default 50%%");
                        } else if (brightness < BRIGHT_VALUE_MIN && brightness > 0) {
                            brightness = BRIGHT_VALUE_MIN;
                        } else if (brightness > BRIGHT_VALUE_MAX) {
                            brightness = BRIGHT_VALUE_MAX;
                        }
                        
                        UINT16_T temp = (UINT16_T)s_random_timing_plan.T * 10;  /* 0-100 -> 0-1000 */
                        if (temp > TEMP_VALUE_MAX) {
                            temp = TEMP_VALUE_MAX;
                        }
                        /* 注意：如果 temp == 0，不调用 light_control_set_color_temp，保持当前色温 */
                        
                        light_control_set(TRUE);
                        light_control_set_brightness(brightness);
                        /* 只有当色温不为0时才设置色温 */
                        if (temp > 0) {
                            light_control_set_color_temp(temp);
                            TAL_PR_NOTICE("[DP210] Random: Light ON - B=%d%%(raw=%d->%d), T=%d%%(raw=%d->%d) (was off for %lu seconds)", 
                                          s_random_timing_plan.B, s_random_timing_plan.B, brightness,
                                          s_random_timing_plan.T, s_random_timing_plan.T, temp, off_duration);
                        } else {
                            TAL_PR_NOTICE("[DP210] Random: Light ON - B=%d%%(raw=%d->%d), T=0(keep current) (was off for %lu seconds)", 
                                          s_random_timing_plan.B, s_random_timing_plan.B, brightness, off_duration);
                        }
                        last_actual_light_state = TRUE;  /* 更新实际状态记录 */
                        next_action_time = now + 2000;  /* 记录设置时间，避免误判 */
                        last_state_change_time = now;  /* 记录状态改变时间 */
                        
                        /* 随机生成亮灯时长 */
                        action_duration = now + MIN_ON_DURATION_MS + 
                                        (rand() % (MAX_ON_DURATION_MS - MIN_ON_DURATION_MS));
                        TAL_PR_NOTICE("[DP210] Random: Will check again in %lu seconds (on duration)", 
                                     (action_duration - now) / 1000);
                    } else {
                        /* 继续灭灯，延长灭灯时长 */
                        action_duration = now + MIN_OFF_DURATION_MS + 
                                        (rand() % (MAX_OFF_DURATION_MS - MIN_OFF_DURATION_MS));
                        TAL_PR_DEBUG("[DP210] Random: Continue OFF, next check in %lu seconds", 
                                    (action_duration - now) / 1000);
                    }
                }
            }
            
            /* 定期检查时间（用于状态日志输出等） */
            if (now >= next_action_time) {
                /* 更新下次检查时间 */
                next_action_time = now + MIN_RANDOM_INTERVAL_MS + 
                                 (rand() % (MAX_RANDOM_INTERVAL_MS - MIN_RANDOM_INTERVAL_MS));
            }
        } else {
            /* 不在有效时间内，关闭灯光 */
            if (light_state) {
                light_state = FALSE;
                light_control_set(FALSE);
                last_actual_light_state = FALSE;  /* 更新实际状态记录 */
                next_action_time = now;  /* 记录设置时间，避免误判 */
                TAL_PR_DEBUG("Random timing: Light OFF (out of time range)");
            }
            next_action_time = 0;
            action_duration = 0;
        }

        tal_system_sleep(5000);  /* 每5秒检查一次 */
    }

    /* 退出时关闭灯光 */
    if (light_state) {
        light_control_set(FALSE);
    }

    s_random_timing_thread_running = FALSE;
    s_random_timing_thread = NULL;
    TAL_PR_NOTICE("Random timing task exited");
}

/**
 * @brief 助眠模式检查任务线程
 * 定期检查所有助眠计划，当时间匹配时自动执行
 */
STATIC VOID __sleep_check_task(VOID_T *arg)
{
    (VOID)arg;
    
    TAL_PR_NOTICE("[SLEEP] Check task started");
    
    /* 输出所有助眠计划的状态 */
    for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
        SLEEP_PLAN_T *plan = &s_sleep_plans[i];
        if (plan->valid) {
            TAL_PR_NOTICE("[SLEEP] Plan #%d: valid=%d, on_off=%d, time=%02d:%02d, date=0x%02x", 
                         i, plan->valid, plan->on_off, plan->hour, plan->minute, plan->date);
        }
    }
    
    while (s_sleep_check_thread_running) {
        /* 如果助眠模式已激活，等待倒计时结束
         * 助眠模式的持续时间由倒计时控制（step * 5分钟）
         * 倒计时任务会在倒计时结束时关闭灯光并退出助眠模式
         * 这里只需要等待，不需要检查时间
         */
        if (s_sleep_mode_active) {
            /* 检查倒计时是否还在运行
             * 注意：s_countdown_active 和 s_countdown_remaining 是同一文件中的 STATIC 变量，可以直接访问
             */
            if (!s_countdown_active || s_countdown_remaining == 0) {
                /* 倒计时已结束（应该由倒计时任务处理，但这里作为备用检查） */
                TAL_PR_NOTICE("[SLEEP] Countdown finished, exiting sleep mode (backup check)");
                
                /* 找到并关闭所有激活的助眠计划 */
                for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
                    SLEEP_PLAN_T *plan = &s_sleep_plans[i];
                    if (plan->valid && plan->on_off) {
                        plan->on_off = FALSE;
                        TAL_PR_NOTICE("[SLEEP] Plan #%d closed (countdown finished)", i);
                    }
                }
                
                /* 关闭灯光 */
                light_control_set(FALSE);
                s_switch_state = FALSE;
                
                /* 退出助眠模式 */
                s_sleep_mode_active = FALSE;
                
                /* 上报状态 */
                __report_dp_states(FALSE);
                
                TAL_PR_NOTICE("[SLEEP] Sleep mode exited, light turned OFF");
                
                /* 退出助眠模式后，继续检查其他计划 */
                continue;
            }
            
            /* 助眠模式激活时，定期输出日志 */
            static UINT32_T last_active_log = 0;
            UINT32_T now = tal_system_get_millisecond();
            if (last_active_log == 0 || now - last_active_log > 60000) {
                TAL_PR_DEBUG("[SLEEP] Sleep mode active, countdown remaining: %d seconds", s_countdown_remaining);
                last_active_log = now;
            }
            tal_system_sleep(10000);  /* 已激活时，每10秒检查一次 */
            continue;
        }
        
        /* 获取当前时间 */
        UINT16_T current_minutes = __get_current_time_minutes();
        if (current_minutes == 0xFFFF) {
            /* 时间无效，等待同步 */
            static UINT32_T last_time_warn = 0;
            UINT32_T now = tal_system_get_millisecond();
            if (last_time_warn == 0 || now - last_time_warn > 30000) {
                TAL_PR_WARN("[SLEEP] Time invalid (0xFFFF), waiting for sync...");
                last_time_warn = now;
            }
            tal_system_sleep(10000);  /* 10秒后重试 */
            continue;
        }
        
        /* 输出当前时间和计划状态（每30秒一次） */
        static UINT32_T last_status_log = 0;
        UINT32_T now = tal_system_get_millisecond();
        if (last_status_log == 0 || now - last_status_log > 30000) {
            TAL_PR_NOTICE("[SLEEP] Current time: %d(%02d:%02d), checking %d plans", 
                         current_minutes, current_minutes/60, current_minutes%60, MAX_SLEEP_PLANS);
            last_status_log = now;
        }
        
        /* 检查所有助眠计划 */
        BOOL_T plan_found = FALSE;
        UINT16_T min_time_to_plan = 1440;  /* 找到距离最近计划的分钟数 */
        
        for (UINT_T i = 0; i < MAX_SLEEP_PLANS; i++) {
            SLEEP_PLAN_T *plan = &s_sleep_plans[i];
            
            /* 跳过无效或关闭的计划 */
            if (!plan->valid || !plan->on_off) {
                continue;
            }
            
            plan_found = TRUE;
            
            /* 计算计划时间（分钟数） */
            UINT16_T plan_minutes = (UINT16_T)(plan->hour * 60 + plan->minute);
            
            /* 计算距离计划时间的分钟数（考虑跨天情况） */
            INT16_T time_diff = (INT16_T)(current_minutes - plan_minutes);
            if (time_diff < 0) {
                time_diff = -time_diff;
            }
            /* 如果时间差大于720分钟（12小时），可能是跨天的情况，需要重新计算 */
            if (time_diff > 720) {
                time_diff = 1440 - time_diff;  /* 1440分钟 = 24小时 */
            }
            
            /* 记录距离最近计划的分钟数 */
            if (time_diff < min_time_to_plan) {
                min_time_to_plan = (UINT16_T)time_diff;
            }
            
            /* 检查当前时间是否匹配计划时间（只允许完全匹配，因为检查间隔已缩短） */
            BOOL_T time_match = (time_diff == 0);
            
            /* 检查星期是否匹配（使用统一的检查函数） */
            BOOL_T week_match = __check_week_match(plan->date);
            
            /* 定期输出检查状态（每30秒一次） */
            static UINT32_T last_check_log = 0;
            UINT32_T now = tal_system_get_millisecond();
            if (last_check_log == 0 || now - last_check_log > 30000) {
                TAL_PR_NOTICE("[SLEEP] Checking plan #%d: Current: %d(%02d:%02d), Plan: %d(%02d:%02d), time_match=%d, week_match=%d", 
                             i, current_minutes, current_minutes/60, current_minutes%60,
                             plan_minutes, plan->hour, plan->minute, time_match, week_match);
                last_check_log = now;
            }
            
            /* 如果时间匹配且星期匹配，执行助眠模式 */
            if (time_match && week_match) {
                TAL_PR_NOTICE("[SLEEP] ========================================");
                TAL_PR_NOTICE("[SLEEP] Plan #%d matched! Current: %d(%02d:%02d), Plan: %d(%02d:%02d)", 
                             i, current_minutes, current_minutes/60, current_minutes%60,
                             plan_minutes, plan->hour, plan->minute);
                
                s_sleep_mode_active = TRUE;  /* 标记助眠模式激活 */
                
                /* 将百分比值转换为0-1000范围 */
                UINT16_T brightness_value = (UINT16_T)plan->B * 10;  /* 0-100 -> 0-1000 */
                if (brightness_value < BRIGHT_VALUE_MIN && brightness_value > 0) {
                    brightness_value = BRIGHT_VALUE_MIN;
                } else if (brightness_value > BRIGHT_VALUE_MAX) {
                    brightness_value = BRIGHT_VALUE_MAX;
                }
                
                UINT16_T temp_value = (UINT16_T)plan->T * 10;  /* 0-100 -> 0-1000 */
                if (temp_value > TEMP_VALUE_MAX) {
                    temp_value = TEMP_VALUE_MAX;
                }
                
                s_brightness_value = brightness_value;
                s_temp_value = temp_value;
                s_switch_state = TRUE;
                
                TAL_PR_NOTICE("[SLEEP] Applying sleep mode: B=%d (from %d%%), T=%d (from %d%%)", 
                              brightness_value, plan->B, temp_value, plan->T);
                
                light_control_set(s_switch_state);
                light_control_set_brightness(s_brightness_value);
                light_control_set_color_temp(temp_value);
                
                TAL_PR_NOTICE("[SLEEP] Sleep mode applied - B: %d, T: %d, Switch: ON", brightness_value, temp_value);
                
                /* 计算持续时间：step * 5分钟 = 秒数 */
                UINT32_T duration = (UINT32_T)plan->step * 5 * 60;  /* step * 5分钟 * 60秒/分钟 */
                if (duration > 0) {
                    TAL_PR_NOTICE("[SLEEP] Sleep mode will last %d seconds (%d steps * 5 minutes)", 
                                 duration, plan->step);
                    __start_countdown(duration);
                }
                
                /* 如果是单次模式（date=0），执行后关闭计划 */
                if (plan->date == 0) {
                    plan->on_off = FALSE;
                    TAL_PR_NOTICE("[SLEEP] Once mode: plan #%d disabled after execution", i);
                }
                
                break;  /* 只执行第一个匹配的计划 */
            }
        }
        
        /* 如果没有找到任何计划，等待更长时间 */
        if (!plan_found) {
            tal_system_sleep(60000);  /* 没有计划时，每分钟检查一次 */
        } else {
            /* 根据距离最近计划的分钟数，动态调整检查间隔：
             * - 如果距离计划时间 < 2分钟，每1秒检查一次（精确执行）
             * - 如果距离计划时间 < 5分钟，每2秒检查一次（接近执行）
             * - 如果距离计划时间 < 15分钟，每5秒检查一次（等待执行）
             * - 否则每10秒检查一次（长时间等待）
             */
            UINT32_T check_interval;
            if (min_time_to_plan < 2) {
                check_interval = 1000;  /* 1秒 */
            } else if (min_time_to_plan < 5) {
                check_interval = 2000;  /* 2秒 */
            } else if (min_time_to_plan < 15) {
                check_interval = 5000;  /* 5秒 */
            } else {
                check_interval = 10000;  /* 10秒 */
            }
            
            tal_system_sleep(check_interval);
        }
    }
    
    s_sleep_check_thread_running = FALSE;
    s_sleep_check_thread = NULL;
    TAL_PR_NOTICE("[SLEEP] Check task exited");
}

/**
 * @brief 更新实时灯光状态并保存到KV
 */
VOID dp_update_realtime_light_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp)
{
    if (!s_realtime_light_save_enabled) {
        return;
    }

    REALTIME_LIGHT_STORAGE_T storage = {0};
    storage.magic = REALTIME_LIGHT_MAGIC;
    storage.switch_on = switch_on ? 1 : 0;

    if (!switch_on) {
        brightness = 0;
    }
    if (brightness > BRIGHT_VALUE_MAX) {
        brightness = BRIGHT_VALUE_MAX;
    }
    storage.brightness = brightness;

    if (color_temp > TEMP_VALUE_MAX) {
        color_temp = TEMP_VALUE_MAX;
    }
    storage.color_temp = color_temp;

    /* 检查数据是否变化 */
    if (s_realtime_light_cache_valid &&
        memcmp(&storage, &s_realtime_light_last, sizeof(storage)) == 0) {
        return;
    }

    /* 节流机制：检查距离上次保存的时间间隔，避免频繁写入KV导致看门狗复位 */
    UINT32_T now_ms = tal_system_get_millisecond();
    if (s_realtime_light_last_save_ms > 0) {
        UINT32_T elapsed_ms = (now_ms >= s_realtime_light_last_save_ms) ? 
                              (now_ms - s_realtime_light_last_save_ms) : 
                              (0xFFFFFFFF - s_realtime_light_last_save_ms + now_ms);
        
        if (elapsed_ms < REALTIME_LIGHT_SAVE_MIN_INTERVAL_MS) {
            /* 距离上次保存时间太短，跳过本次保存，避免频繁写入KV */
            TAL_PR_DEBUG("[REALTIME_LIGHT] Save throttled (elapsed=%lu ms < %d ms)",
                         (unsigned long)elapsed_ms, REALTIME_LIGHT_SAVE_MIN_INTERVAL_MS);
            return;
        }
    }

    /* 执行KV写入 */
    OPERATE_RET rt = wd_common_write(KV_KEY_REALTIME_LIGHT,
                                     (BYTE_T *)&storage,
                                     sizeof(storage));
    if (rt == OPRT_OK) {
        s_realtime_light_last = storage;
        s_realtime_light_cache_valid = TRUE;
        s_realtime_light_last_save_ms = now_ms;
        TAL_PR_DEBUG("[REALTIME_LIGHT] Saved switch=%d brightness=%d temp=%d",
                     storage.switch_on, storage.brightness, storage.color_temp);
    } else {
        TAL_PR_ERR("[REALTIME_LIGHT] Save failed: %d", rt);
        /* 写入失败时也更新时间戳，避免连续失败导致频繁重试 */
        s_realtime_light_last_save_ms = now_ms;
    }
}

/**
 * @brief 控制实时灯光状态保存开关
 */
VOID dp_enable_realtime_light_save(BOOL_T enable)
{
    s_realtime_light_save_enabled = enable ? TRUE : FALSE;
    TAL_PR_DEBUG("[REALTIME_LIGHT] Save %s", enable ? "ENABLED" : "DISABLED");
}
