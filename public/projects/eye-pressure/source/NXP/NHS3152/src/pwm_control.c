/*
 * PWM控制模块 - 通过NFC命令控制PIO0-3输出PWM
 */

 #include "board.h"
 #include <stdint.h>
 #include <stdbool.h>
 #include <string.h>
 
 // PWM控制命令定义
 #define PWM_CMD_INIT    0x01  // 初始化PWM
 #define PWM_CMD_START   0x02  // 启动PWM
 #define PWM_CMD_STOP    0x03  // 停止PWM
 #define PWM_CMD_SET_DUTY 0x04 // 设置占空比
 #define PWM_CMD_SET_FREQ 0x05 // 设置频率
 
 // PWM参数结构
 typedef struct {
     uint8_t cmd;        // 命令码
     uint8_t freq_high;  // 频率高字节
     uint8_t freq_low;   // 频率低字节
     uint8_t duty;       // 占空比 (0-100)
 } pwm_control_t;
 
 // PWM状态变量
 static bool sPwmInitialized = false;
 static bool sPwmRunning = false;
 static uint32_t sCurrentFreq = 2000;  // 默认频率2000Hz
 static uint8_t sCurrentDuty = 50;     // 默认占空比50%

 // 状态保存变量
 static uint32_t sSavedFreq = 2000;
 static uint8_t sSavedDuty = 50;
 static bool sSavedRunning = false;
 
 // 调试变量
 static uint32_t sNfcCommandCount = 0;
 static uint32_t sNfcCommandFailCount = 0;
 
 // 导出上次命令数组供外部访问
 uint8_t sLastNfcCommand[4] = {0};
 
 // 前向声明
 static void pwm_set_parameters(uint32_t freq, uint8_t duty);
 static void pwm_init(void);
 static void pwm_save_state(void);
 static void pwm_restore_state(void);
 void pwm_start(void);
 void pwm_stop(void);
 void pwm_set_duty_cycle(uint8_t duty_cycle);
void pwm_set_frequency(uint32_t freq_hz);
void pwm_apply_duty_low_power(uint8_t duty);
 
 /**
  * 公共PWM模块初始化函数
  */
 bool pwm_init_module(void)
 {
     pwm_init();
     
     // 恢复保存的状态
     pwm_restore_state();
     
     return sPwmInitialized;
 }
 
 /**
  * 初始化PWM输出
  */
 static void pwm_init(void)
 {
     // PIO0_3 → CT16B0_MAT0（PWM 通道 0）
     // 禁用上下拉（INACT），改成 FUNC1
     Chip_IOCON_SetPinConfig(NSS_IOCON,
                             IOCON_PIO0_3,
                             IOCON_FUNC_1 | IOCON_RMODE_INACT);
     Chip_TIMER16_0_Init();
     
     // 确认引脚配置
     __attribute__((unused)) int pinConfig = Chip_IOCON_GetPinConfig(NSS_IOCON, IOCON_PIO0_3);
     
     sPwmInitialized = true;
     sPwmRunning = false;
 }
 
 /**
  * 验证占空比计算
  */
static void pwm_verify_duty_calculation(uint8_t duty_percent, uint32_t pwm_period)
{
    uint32_t duty_counts = (pwm_period * duty_percent) / 100;
    
    // 调试信息：验证占空比计算
    __attribute__((unused)) uint8_t debug_duty_percent = duty_percent;
    __attribute__((unused)) uint32_t debug_pwm_period = pwm_period;
    __attribute__((unused)) uint32_t debug_duty_counts = duty_counts;
    __attribute__((unused)) uint32_t debug_mr0_value = duty_counts - 1;
    
    // 验证计算
    if (duty_percent == 25) {
        // 25% 应该等于 25 counts
        __attribute__((unused)) uint32_t expected_counts = 25;
        __attribute__((unused)) bool calculation_correct = (duty_counts == expected_counts);
    }
}

/**
 * 设置PWM参数
 */
static void pwm_set_parameters(uint32_t freq, uint8_t duty)
{
    if (!sPwmInitialized) return;
    
    Chip_TIMER_Disable(NSS_TIMER16_0);
    
    // 预分频：让 TC 以 freq * period_counts 速率计数
    uint32_t systemClock = (uint32_t)Chip_Clock_System_GetClockFreq();
    uint32_t pwmPeriod = 100; // 100个计数为一个周期
    uint32_t prescale = systemClock / (freq * pwmPeriod) - 1;
    if (prescale > 0xFFFF) prescale = 0xFFFF;
    Chip_TIMER_PrescaleSet(NSS_TIMER16_0, prescale);
    
    // 验证占空比计算
    pwm_verify_duty_calculation(duty, pwmPeriod);
    
    // 占空比计算：duty_counts = (pwmPeriod * duty) / 100
    uint32_t duty_counts = (pwmPeriod * duty) / 100;
    if (duty_counts == 0) duty_counts = 1; // 避免0，使PWM仍输出最小脉宽
    if (duty_counts > pwmPeriod) duty_counts = pwmPeriod; // 占空比不能超过100%
    
    // MR0 = duty_counts - 1 (占空比控制)
    Chip_TIMER_SetMatch(NSS_TIMER16_0, 0, duty_counts - 1);
    Chip_TIMER_MatchDisableInt(NSS_TIMER16_0, 0);
    Chip_TIMER_StopOnMatchDisable(NSS_TIMER16_0, 0);
    Chip_TIMER_ResetOnMatchDisable(NSS_TIMER16_0, 0);
    
    // MR1 -> 不使用
    Chip_TIMER_SetMatch(NSS_TIMER16_0, 1, pwmPeriod);
    Chip_TIMER_MatchDisableInt(NSS_TIMER16_0, 1);
    Chip_TIMER_StopOnMatchDisable(NSS_TIMER16_0, 1);
    Chip_TIMER_ResetOnMatchDisable(NSS_TIMER16_0, 1);
    
    // MR2 = pwmPeriod - 1 （复位周期）
    Chip_TIMER_SetMatch(NSS_TIMER16_0, 2, pwmPeriod - 1);
    Chip_TIMER_MatchDisableInt(NSS_TIMER16_0, 2);
    Chip_TIMER_StopOnMatchDisable(NSS_TIMER16_0, 2);
    Chip_TIMER_ResetOnMatchEnable(NSS_TIMER16_0, 2);
    
    // MR3 -> 不使用
    Chip_TIMER_MatchDisableInt(NSS_TIMER16_0, 3);
    Chip_TIMER_StopOnMatchDisable(NSS_TIMER16_0, 3);
    Chip_TIMER_ResetOnMatchDisable(NSS_TIMER16_0, 3);
    
    // 启用PWM模式（根据官方例程）
    Chip_TIMER_SetMatchOutputMode(NSS_TIMER16_0, 0, TIMER_MATCH_OUTPUT_PWM);
    
    sCurrentFreq = freq;
    sCurrentDuty = duty;
    
    // 保存状态
    pwm_save_state();
    
    // 启动定时器
    Chip_TIMER_Reset(NSS_TIMER16_0);
    Chip_TIMER_Enable(NSS_TIMER16_0);
    
    // 调试信息：记录设置参数
    __attribute__((unused)) uint32_t debug_freq = freq;
    __attribute__((unused)) uint8_t debug_duty = duty;
    __attribute__((unused)) uint32_t debug_duty_counts = duty_counts;
    __attribute__((unused)) uint32_t debug_prescale = prescale;
    __attribute__((unused)) uint32_t debug_mr0 = NSS_TIMER16_0->MR[0];
    __attribute__((unused)) uint32_t debug_mr2 = NSS_TIMER16_0->MR[2];
    __attribute__((unused)) uint32_t debug_emr = NSS_TIMER16_0->EMR;
    __attribute__((unused)) uint32_t debug_system_clock = systemClock;
    __attribute__((unused)) uint32_t debug_pwm_period = pwmPeriod;
}
 
 /**
  * 启动PWM输出
  */
 void pwm_start(void)
 {
     if (sPwmInitialized) {
         // 确保引脚配置为定时器功能
         Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_PIO0_3, IOCON_FUNC_1 | IOCON_RMODE_INACT);
         
         // 确保定时器已启用
         Chip_TIMER_Enable(NSS_TIMER16_0);
         sPwmRunning = true;
         
        // 保存状态（去掉忙等待与多余寄存器访问以降功耗）
        pwm_save_state();
     }
 }
 
 /**
  * 停止PWM输出（将引脚设置为低电平）
  */
 void pwm_stop(void)
 {
     if (sPwmInitialized) {
         // 停止定时器
         Chip_TIMER_Disable(NSS_TIMER16_0);
         sPwmRunning = false;
         
         // 将引脚设置为GPIO模式并输出低电平
         Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_PIO0_3, IOCON_FUNC_0);
         Chip_GPIO_SetPinDIROutput(NSS_GPIO, 0, 3);
         Chip_GPIO_SetPinState(NSS_GPIO, 0, 3, false); // 输出低电平
         
         // 保存状态
         pwm_save_state();
     }
 }
 
 /**
  * 设置PWM占空比
  */
 void pwm_set_duty_cycle(uint8_t duty_cycle)
 {
     if (duty_cycle <= 100) {
         pwm_set_parameters(sCurrentFreq, duty_cycle);
         if (sPwmRunning) {
             pwm_start();
         }
     }
 }

void pwm_set_frequency(uint32_t freq_hz)
{
    if (freq_hz == 0 || freq_hz > 100000) return;
    sCurrentFreq = freq_hz;
    pwm_set_parameters(sCurrentFreq, sCurrentDuty);
    if (sPwmRunning) {
        pwm_start();
    }
}

/* 低功耗占空比应用：0%/100%转静态电平，其余运行PWM */
void pwm_apply_duty_low_power(uint8_t duty)
{
    if (duty > 100) return;
    if (!sPwmInitialized) pwm_init();

    if (duty == 0) {
        pwm_stop();
        // 输出低电平
        Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_PIO0_3, IOCON_FUNC_0);
        Chip_GPIO_SetPinDIROutput(NSS_GPIO, 0, 3);
        Chip_GPIO_SetPinState(NSS_GPIO, 0, 3, false);
        sCurrentDuty = 0;
        pwm_save_state();
        return;
    }
    if (duty == 100) {
        pwm_stop();
        // 输出高电平
        Chip_IOCON_SetPinConfig(NSS_IOCON, IOCON_PIO0_3, IOCON_FUNC_0);
        Chip_GPIO_SetPinDIROutput(NSS_GPIO, 0, 3);
        Chip_GPIO_SetPinState(NSS_GPIO, 0, 3, true);
        sCurrentDuty = 100;
        pwm_save_state();
        return;
    }

    pwm_set_parameters(sCurrentFreq, duty);
    pwm_start();
}
 
 /**
  * 处理NFC PWM命令
  * 命令格式: [前缀] [频率高字节] [频率低字节] [占空比]
  * 例如: 0x02 0x07 0xD0 0x19 表示频率2000Hz，占空比25%
  *      0x00 0x00 0x00 0x00 表示停止PWM（输出0V）
  */
 void pwm_handle_nfc_command(const uint8_t* command)
 {
     if (!command) return;
     
     sNfcCommandCount++;
     memcpy(sLastNfcCommand, command, 4);
     
     uint8_t prefix = command[0];
     
     // 调试信息：记录接收到的命令
     __attribute__((unused)) uint8_t debug_cmd0 = command[0];
     __attribute__((unused)) uint8_t debug_cmd1 = command[1];
     __attribute__((unused)) uint8_t debug_cmd2 = command[2];
     __attribute__((unused)) uint8_t debug_cmd3 = command[3];
     
     // 处理停止命令
     if (prefix == 0x00) {
         // 停止PWM（输出0V）
         pwm_stop();
         return;
     }
     
     // 处理设置PWM参数的命令
     if (prefix == 0x02) {
         uint32_t freq = ((uint32_t)command[1] << 8) | command[2];
         uint8_t duty = command[3];
         
         __attribute__((unused)) uint32_t debug_parsed_freq = freq;
         __attribute__((unused)) uint8_t debug_parsed_duty = duty;
         __attribute__((unused)) uint8_t debug_raw_duty = command[3];
         __attribute__((unused)) uint8_t debug_raw_freq_high = command[1];
         __attribute__((unused)) uint8_t debug_raw_freq_low = command[2];
         
         // 检查是否为停止命令
         if (freq == 0 && duty == 0) {
             // 停止PWM
             pwm_stop();
         } else {
             // 设置PWM参数
             // 参数验证
             if (freq > 0 && freq <= 100000 && duty <= 100) {
                 pwm_set_parameters(freq, duty);
                 pwm_start();
             } else {
                 sNfcCommandFailCount++;
             }
         }
     } else {
         sNfcCommandFailCount++;
     }
 }
 
 /**
  * 获取NFC命令调试信息
  */
 void pwm_get_nfc_command_debug(uint32_t* commandCount, uint32_t* failCount, uint8_t* lastCommand)
 {
     if (commandCount) *commandCount = sNfcCommandCount;
     if (failCount) *failCount = sNfcCommandFailCount;
     if (lastCommand) memcpy(lastCommand, sLastNfcCommand, 4);
 }
 
 /**
  * 获取当前PWM状态
  */
 bool pwm_get_status(bool* running, uint32_t* freq, uint8_t* duty)
 {
     if (!sPwmInitialized) {
         if (running) *running = false;
         return false;
     }
     if (freq) *freq = sCurrentFreq;
     if (duty) *duty = sCurrentDuty;
     if (running) *running = sPwmRunning;
     return true;
 }

/**
 * 获取定时器调试信息
 */
void pwm_get_timer_debug(uint32_t* tc, uint32_t* prescale, uint32_t* mr0, uint32_t* mr1)
{
    if (tc) *tc = Chip_TIMER_ReadCount(NSS_TIMER16_0);
    if (prescale) *prescale = Chip_TIMER_ReadPrescale(NSS_TIMER16_0);
    if (mr0) *mr0 = NSS_TIMER16_0->MR[0];
    if (mr1) *mr1 = NSS_TIMER16_0->MR[1];
}

/**
 * 获取外部匹配寄存器状态
 */
uint32_t pwm_get_extmatch_status(void)
{
    return NSS_TIMER16_0->EMR;
}

/**
 * 保存PWM状态
 */
static void pwm_save_state(void)
{
    sSavedFreq = sCurrentFreq;
    sSavedDuty = sCurrentDuty;
    sSavedRunning = sPwmRunning;
}

/**
 * 恢复PWM状态
 */
static void pwm_restore_state(void)
{
    sCurrentFreq = sSavedFreq;
    sCurrentDuty = sSavedDuty;
    if (sSavedRunning && sPwmInitialized) {
        pwm_set_parameters(sCurrentFreq, sCurrentDuty);
        pwm_start();
    }
}