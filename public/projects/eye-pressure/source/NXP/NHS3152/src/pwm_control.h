/*
 * PWM控制模块头文件
 */

 #ifndef PWM_CONTROL_H
 #define PWM_CONTROL_H
 
 #include <stdint.h>
 #include <stdbool.h>
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /**
  * 初始化PWM模块
  * @return true: 初始化成功, false: 初始化失败
  */
 bool pwm_init_module(void);
 
 /**
  * 启动PWM输出
  */
 void pwm_start(void);
 
 /**
  * 停止PWM输出
  */
 void pwm_stop(void);
 
 /**
  * 设置PWM占空比
  * @param duty_cycle 占空比 (0-100)
  */
 void pwm_set_duty_cycle(uint8_t duty_cycle);

 /**
  * 低功耗方式应用占空比：
  * - 0%: 停止PWM并输出低电平
  * - 100%: 停止PWM并输出高电平
  * - 1..99%: 以当前频率开启PWM
  */
 void pwm_apply_duty_low_power(uint8_t duty);

 /**
  * 设置PWM频率（Hz）。若PWM正在运行，则即时生效。
  */
 void pwm_set_frequency(uint32_t freq_hz);
 
 /**
  * 处理NFC PWM命令
  * @param command 4字节命令数据
 */
void pwm_handle_nfc_command(const uint8_t* command);
 
 /**
  * 获取当前PWM状态
  * @param running 返回PWM是否正在输出
  * @param freq 返回当前频率
  * @param duty 返回当前占空比
  * @return true: PWM已初始化, false: PWM未初始化
  */
 bool pwm_get_status(bool* running, uint32_t* freq, uint8_t* duty);
 
 /**
  * 获取NFC命令调试信息
  * @param commandCount 返回总命令数
  * @param failCount 返回失败命令数
  * @param lastCommand 返回最后一个命令
  */
 void pwm_get_nfc_command_debug(uint32_t* commandCount, uint32_t* failCount, uint8_t* lastCommand);

 /**
  * 获取定时器调试信息
  * @param tc 返回定时器计数值
  * @param prescale 返回预分频器值
  * @param mr0 返回匹配寄存器0值
  * @param mr1 返回匹配寄存器1值
  */
 void pwm_get_timer_debug(uint32_t* tc, uint32_t* prescale, uint32_t* mr0, uint32_t* mr1);

 /**
  * 获取外部匹配寄存器状态
  * @return 外部匹配寄存器值
  */
 uint32_t pwm_get_extmatch_status(void);
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif // PWM_CONTROL_H