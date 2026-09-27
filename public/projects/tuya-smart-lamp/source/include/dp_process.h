/**
 * @file dp_process.h
 * @author www.tuya.com
 * @brief dp_process module is used to 
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#ifndef __DP_PROCESS_H__
#define __DP_PROCESS_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/


/***********************************************************
***********************typedef define***********************
***********************************************************/


/***********************************************************
********************function declaration********************
***********************************************************/
/**
 * @brief Output the received data and reply to the cloud
 *
 * @param[in] dp_data_arr: the array of recevie dp
 * @param[in] dp_cnt: the number of dp
 *
 * @return none
 */
VOID dp_obj_process(CONST TY_OBJ_DP_S *dp_data_arr, UINT_T dp_cnt);

/**
 * @brief Output the received raw type data and reply to the cloud
 *
 * @param[in] dpid: received raw dp id
 * @param[in] p_data: raw dp data
 * @param[in] data_len: the length of data
 * 
 * @return none
 */
VOID dp_raw_process(UINT8_T dpid, CONST UINT8_T *p_data, UINT_T data_len);

/**
 * @brief   upload swtich status
 *
 * @param[in] : state   the state of switch
 *
 * @return none
 */
VOID upload_device_switch_status(BOOL_T state);

/**
 * @brief report all dp to the cloud
 *
 * @param[in] none: 
 *
 * @return none
 */
VOID upload_device_all_status(VOID_T);

/**
 * @brief respone all dp status when receive query from cloud or app
 *
 * @param[in] none: 
 *
 * @return none
 */
VOID_T respone_device_all_status(VOID_T);

/**
 * @brief 获取开关状态
 * @return 开关状态（TRUE=开，FALSE=关）
 */
BOOL_T dp_get_switch_state(VOID);

/**
 * @brief 获取亮度值
 * @return 亮度值（10-1000）
 */
UINT16_T dp_get_brightness_value(VOID);

/**
 * @brief 获取色温值
 * @return 色温值（0-1000）
 */
UINT16_T dp_get_temp_value(VOID);

/**
 * @brief 等待断电记忆DP 33下发（用于上电恢复）
 * @param timeout_ms 超时时间（毫秒）
 * @return TRUE=已收到DP 33且断电记忆已启用，FALSE=超时或未启用
 */
BOOL_T dp_wait_power_memory(UINT32_T timeout_ms);

/**
 * @brief 检查断电记忆是否已启用
 * @return TRUE=已启用，FALSE=未启用
 */
BOOL_T dp_is_power_memory_enabled(VOID);

/**
 * @brief 检查是否已收到断电记忆DP 33
 * @return TRUE=已收到，FALSE=未收到
 */
BOOL_T dp_is_power_memory_received(VOID);

/**
 * @brief 获取通电勿扰状态
 * @return TRUE=已开启，FALSE=未开启
 */
BOOL_T dp_get_do_not_disturb_state(VOID);

/**
 * @brief 检查通电勿扰的上电次数（供外部调用）
 * @return TRUE=需要延迟初始化（未达到要求的上电次数），FALSE=可以正常初始化
 */
BOOL_T dp_check_do_not_disturb_power_cycle(VOID);

/**
 * @brief 延迟保存通电勿扰的上电次数（如果标记需要保存）
 * @note 在恢复灯光后调用，避免在关键路径上立即写入KV导致看门狗复位
 */
VOID dp_dnd_power_cycle_delayed_save(VOID);

/**
 * @brief 延迟保存通电勿扰状态（如果标记需要保存）
 * @note 在后台任务中调用，避免在DP处理的关键路径上立即写入KV导致看门狗复位
 */
VOID dp_dnd_state_delayed_save(VOID);

/**
 * @brief 应用缓存的断电记忆设置
 * @return TRUE=已应用，FALSE=无有效配置
 */
BOOL_T dp_apply_cached_power_memory(VOID);

/**
 * @brief 更新实时灯光状态并保存
 */
VOID dp_update_realtime_light_state(BOOL_T switch_on, UINT16_T brightness, UINT16_T color_temp);

/**
 * @brief 启用/禁用实时灯光状态保存
 */
VOID dp_enable_realtime_light_save(BOOL_T enable);


#ifdef __cplusplus
}
#endif

#endif /* __DP_PROCESS_H__ */
