#include "app.h"
#include "uart.h"
#include "modbus.h"
#include "key.h"
#include "cmd_queue.h"
#include "systick.h"
#include "laser.h"
#include "dht11.h"
#include "st7789.h"
#include "ui.h"
#include "ui_theme.h"
#include "delay.h"
#include "STC8H.h"  // For PWMA_PS register definition
#include "STC8G_PWM15bit.h"  // For PWMSET register and PWM15_PWM4_Set macro
#include "font_ascii_8x16.h"  // For Font_Get32x32_Chinese function
#include "gp8202.h"            // GP8202AS DAC for 4-20mA current output

// Global variable for screen activity timer (shared between App_ProcessLCD and App_MainLoop)
static unsigned long app_last_activity_tick = 0;

// Global flag to clear manual countdown mode when measurement succeeds
unsigned char auto_measure_manual_countdown_active_flag = 0;

// Page switching state: 0 = page 1 (sensor data), 1 = page 2 (contact info)
static unsigned char current_page = 0;

// Info page timeout (1 minute = 60000ms)
#define INFO_PAGE_TIMEOUT_MS 60000
static unsigned long info_page_start_tick = 0;  // Info page display start time
static unsigned char info_page_active = 0;      // Info page active flag

// Screen blackout state (shared with App_ProcessLCD)
static unsigned char app_screen_blackout = 0;   // 0=screen on, 1=screen off

// ===== 料位起止点设置状态管理 =====
// 设置状态枚举
typedef enum {
    SETTING_STATE_IDLE = 0,              // 正常工作状态
    SETTING_STATE_START_MANUAL,          // 起点手动设置状态
    SETTING_STATE_START_LASER,           // 起点激光标定状态
    SETTING_STATE_END_MANUAL,            // 终点手动设置状态
    SETTING_STATE_END_LASER,             // 终点激光标定状态
    SETTING_STATE_ADDR_MANUAL            // 地址手动设置状态
} SettingState;

static SettingState setting_state = SETTING_STATE_IDLE;  // 当前设置状态
static unsigned long setting_start_tick = 0;              // 进入设置状态的时间戳
static unsigned long last_setting_activity_tick = 0;      // 最后一次设置操作的时间戳
static unsigned short temp_start_point = 0;               // 临时起点值(mm)
static unsigned short temp_end_point = 0;                 // 临时终点值(mm)
static unsigned char temp_addr = 0;                       // 临时地址值(1-247)
static unsigned char setting_modified = 0;                // 设置是否被修改过（0=未修改，1=已修改）

#define SETTING_TIMEOUT_MS 120000  // 设置状态超时时间（2分钟）

// K5/K6长按加速参数调整
static unsigned long key_adjust_last_tick = 0;      // 上次参数调整的时间戳
static unsigned short key_adjust_interval_ms = 100; // 当前调整间隔（毫秒）
static unsigned char key_adjust_first = 1;          // 是否是首次调整（用于加速逻辑）
static unsigned char key_adjust_count = 0;          // 连续调整次数（用于加速计算）

#define KEY_ADJUST_INTERVAL_MIN 10   // 最小调整间隔（0.01秒 = 100Hz，超丝滑）
#define KEY_ADJUST_INTERVAL_MAX 100  // 最大调整间隔（0.1秒，初始反应更快）
#define KEY_ADJUST_ACCEL_STEP 2      // 每2次调整加速一次（更快达到最高速）

// UART4 response buffer for command queue
static xdata char uart4_rx_buf[64];
static unsigned char uart4_rx_idx = 0;

/**
 * 进入设置状态
 * @param new_state: 要进入的设置状态
 */
static void App_EnterSettingState(SettingState new_state)
{
    unsigned long now = Systick_GetTick();
    
    setting_state = new_state;
    setting_start_tick = now;
    last_setting_activity_tick = now;
    setting_modified = 0;
    
    // 读取当前起点、终点和地址值作为临时值
    temp_start_point = Modbus_GetHoldingReg(4003);  // 起点寄存器
    temp_end_point = Modbus_GetHoldingReg(4004);    // 终点寄存器
    temp_addr = Modbus_GetHoldingReg(3000) & 0xFF;  // 地址寄存器（低字节）
    
    // 地址范围检查：确保在1-247范围内
    if(temp_addr < 1 || temp_addr > 247)
    {
        temp_addr = 1;  // 默认地址
    }
    
    // 重置UI缓存，确保参数以红色显示
    UI_ResetCache();
    
    // 重置屏幕活动计时器
    App_ResetActivityTimer();
}

/**
 * 退出设置状态并保存参数
 */
static void App_ExitSettingState(void)
{
    // 如果参数被修改过，保存到MODBUS寄存器
    if(setting_modified)
    {
        if(setting_state == SETTING_STATE_START_MANUAL || setting_state == SETTING_STATE_START_LASER)
        {
            // 保存起点参数
            Modbus_SetHoldingReg(4003, temp_start_point);
            // 保存到EEPROM
            Modbus_SaveToEEPROM();
        }
        else if(setting_state == SETTING_STATE_END_MANUAL || setting_state == SETTING_STATE_END_LASER)
        {
            // 保存终点参数
            Modbus_SetHoldingReg(4004, temp_end_point);
            // 保存到EEPROM
            Modbus_SaveToEEPROM();
        }
        else if(setting_state == SETTING_STATE_ADDR_MANUAL)
        {
            // 保存地址参数（立即生效 + 掉电保存到EEPROM）
            Modbus_SetHoldingReg(3000, temp_addr);
            Modbus_SaveToEEPROM();
        }
    }
    
    // 退出设置状态
    setting_state = SETTING_STATE_IDLE;
    setting_modified = 0;
    
    // 重置UI缓存，恢复正常颜色显示
    UI_ResetCache();
    
    // 重置屏幕活动计时器
    App_ResetActivityTimer();
}

/**
 * 更新设置活动时间戳（用于2分钟超时检测）
 */
static void App_UpdateSettingActivity(void)
{
    last_setting_activity_tick = Systick_GetTick();
}

/**
 * 处理K5/K6长按加速调整参数
 * 在主循环中调用，检测K5/K6的长按状态并快速调整参数
 * 优化版本：更快的初始响应，更丝滑的加速曲线
 */
static void App_ProcessLongPressAdjust(unsigned long now)
{
    unsigned char key5_pressed = (readKey(4) == 0);  // KEY5 = key_id 4
    unsigned char key6_pressed = (readKey(5) == 0);  // KEY6 = key_id 5
    
    // 只在手动设置状态下处理长按加速
    if(setting_state != SETTING_STATE_START_MANUAL && 
       setting_state != SETTING_STATE_END_MANUAL &&
       setting_state != SETTING_STATE_ADDR_MANUAL)
    {
        key_adjust_first = 1;  // 重置首次标志
        key_adjust_count = 0;  // 重置调整计数
        return;
    }
    
    // K5长按：递减加速
    if(key5_pressed)
    {
        if(key_adjust_first)
        {
            // 首次调整：初始间隔200ms（更快响应）
            key_adjust_interval_ms = KEY_ADJUST_INTERVAL_MAX;
            key_adjust_last_tick = now;
            key_adjust_first = 0;
            key_adjust_count = 0;
        }
        
        // 检查是否到达调整间隔（使用当前时间而非累加，避免卡顿）
        if(Systick_Elapsed(key_adjust_last_tick, key_adjust_interval_ms))
        {
            // 执行递减
            if(setting_state == SETTING_STATE_START_MANUAL && temp_start_point > 0)
            {
                temp_start_point--;
                setting_modified = 1;
                App_UpdateSettingActivity();
            }
            else if(setting_state == SETTING_STATE_END_MANUAL && temp_end_point > 0)
            {
                temp_end_point--;
                setting_modified = 1;
                App_UpdateSettingActivity();
            }
            else if(setting_state == SETTING_STATE_ADDR_MANUAL)
            {
                // 地址递减：1->247循环
                if(temp_addr > 1)
                {
                    temp_addr--;
                }
                else
                {
                    temp_addr = 247;  // 从1递减到247
                }
                setting_modified = 1;
                App_UpdateSettingActivity();
            }
            
            // 更新时间戳为当前时间（避免累加误差）
            key_adjust_last_tick = now;
            key_adjust_count++;
            
            // 加速逻辑：每调整3次，间隔减少25%（更丝滑的加速曲线）
            if(key_adjust_count >= KEY_ADJUST_ACCEL_STEP)
            {
                key_adjust_count = 0;
                if(key_adjust_interval_ms > KEY_ADJUST_INTERVAL_MIN)
                {
                    // 减少25%：更平滑的加速
                    key_adjust_interval_ms = (key_adjust_interval_ms * 3) / 4;
                    if(key_adjust_interval_ms < KEY_ADJUST_INTERVAL_MIN)
                    {
                        key_adjust_interval_ms = KEY_ADJUST_INTERVAL_MIN;
                    }
                }
            }
        }
    }
    // K6长按：递加加速
    else if(key6_pressed)
    {
        if(key_adjust_first)
        {
            // 首次调整：初始间隔200ms（更快响应）
            key_adjust_interval_ms = KEY_ADJUST_INTERVAL_MAX;
            key_adjust_last_tick = now;
            key_adjust_first = 0;
            key_adjust_count = 0;
        }
        
        // 检查是否到达调整间隔（使用当前时间而非累加，避免卡顿）
        if(Systick_Elapsed(key_adjust_last_tick, key_adjust_interval_ms))
        {
            // 执行递加
            if(setting_state == SETTING_STATE_START_MANUAL && temp_start_point < 65535)
            {
                temp_start_point++;
                setting_modified = 1;
                App_UpdateSettingActivity();
            }
            else if(setting_state == SETTING_STATE_END_MANUAL && temp_end_point < 65535)
            {
                temp_end_point++;
                setting_modified = 1;
                App_UpdateSettingActivity();
            }
            else if(setting_state == SETTING_STATE_ADDR_MANUAL)
            {
                // 地址递加：247->1循环
                if(temp_addr < 247)
                {
                    temp_addr++;
                }
                else
                {
                    temp_addr = 1;  // 从247递加到1
                }
                setting_modified = 1;
                App_UpdateSettingActivity();
            }
            
            // 更新时间戳为当前时间（避免累加误差）
            key_adjust_last_tick = now;
            key_adjust_count++;
            
            // 加速逻辑：每调整3次，间隔减少25%（更丝滑的加速曲线）
            if(key_adjust_count >= KEY_ADJUST_ACCEL_STEP)
            {
                key_adjust_count = 0;
                if(key_adjust_interval_ms > KEY_ADJUST_INTERVAL_MIN)
                {
                    // 减少25%：更平滑的加速
                    key_adjust_interval_ms = (key_adjust_interval_ms * 3) / 4;
                    if(key_adjust_interval_ms < KEY_ADJUST_INTERVAL_MIN)
                    {
                        key_adjust_interval_ms = KEY_ADJUST_INTERVAL_MIN;
                    }
                }
            }
        }
    }
    else
    {
        // 按键释放：重置所有状态
        key_adjust_first = 1;
        key_adjust_interval_ms = KEY_ADJUST_INTERVAL_MAX;
        key_adjust_count = 0;
    }
}

/**
 * Process UART4 responses for command queue
 * Receives characters from UART4 interrupt buffer and assembles complete frames
 * Laser module responses end with '>' character
 * Optimized: Process multiple characters per call to avoid blocking
 */
void App_ProcessUart4Response(void)
{
    int recv_char;
    unsigned char processed = 0;
    
    // Process multiple characters per call (up to 5) to avoid blocking
    // Reduced from 20 to 5 to improve key responsiveness
    while(processed < 5)
    {
        recv_char = Uart4_GetCharFromBuffer();
        if(recv_char < 0)
        {
            break;  // No more data available
        }
        
        if(uart4_rx_idx < sizeof(uart4_rx_buf) - 1)
        {
            // Add character to buffer
            uart4_rx_buf[uart4_rx_idx++] = (unsigned char)recv_char;
            
            // Check for end of frame (laser module uses '>' as terminator)
            if(recv_char == '>')
            {
                // Complete frame received, null-terminate and process
                uart4_rx_buf[uart4_rx_idx] = '\0';
                CmdQueue_HandleResponse(uart4_rx_buf);
                uart4_rx_idx = 0;  // Reset buffer index
                processed++;
                // Continue processing more characters if available
            }
            else
            {
                processed++;
            }
        }
        else
        {
            // Buffer overflow, reset
            uart4_rx_idx = 0;
            break;
        }
    }
}

/**
 * Trigger UART batch sending for all UARTs
 * Checks if each UART is idle and has data to send, then triggers batch send
 */
void App_TriggerUartSending(void)
{
    // Trigger UART0 batch send if idle and has data
    if(UART0_IsSending() == 0 && UART0_HasDataToSend())
    {
        UART0_SendBatch();
    }
    
    // Trigger UART3 batch send if idle and has data
    // Enabled: UART3 used for MODBUS RTU communication with NE2 module
    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
    {
        Uart3_SendBatch();
    }
    
    // Trigger UART4 batch send if idle and has data
    if(Uart4_IsSending() == 0 && Uart4_HasDataToSend())
    {
        Uart4_SendBatch();
    }
}

/**
 * Process UART passthrough functions
 * Bidirectional data forwarding between UARTs
 * 
 * Passthrough options:
 * - UART3 <-> UART0: NE2 module <-> Debug port
 *   Note: This may interfere with MODBUS communication on UART3
 *   Enable only when MODBUS is not needed
 * 
 * - UART0 <-> UART4: Debug port <-> Laser module
 *   Note: This may interfere with command queue processing on UART4
 *   Enable only when command queue is not needed
 * 
 * To enable passthrough, uncomment the corresponding function call below.
 */
#if 0
void App_ProcessPassthrough(void)
{
    // Passthrough: UART3 <-> UART0 (NE2 module <-> Debug port)
    // Uncomment to enable: NE2 module data will be forwarded to debug port and vice versa
    // WARNING: This will disable MODBUS processing on UART3
    // UART_Passthrough_UART3_UART0();
    
    // Passthrough: UART0 <-> UART4 (Debug port <-> Laser module)
    // DISABLED: Conflicts with command queue processing on UART4
    // Both App_ProcessUart4Response() and UART_Passthrough_UART0_UART4() read from UART4 RX buffer
    // Enabling passthrough will cause command queue to fail (data will be forwarded instead of parsed)
    // To enable passthrough: uncomment below and comment out App_ProcessUart4Response() in App_MainLoop()
    // UART_Passthrough_UART0_UART4();
}
#endif

/**
 * Process DHT11 sensor reading (non-blocking)
 * Reads DHT11 data every 2.5 seconds and updates MODBUS registers
 * MODBUS Register Mapping:
 * - 3001: Temperature (high byte: integer+sign, low byte: decimal)
 * - 3002: Humidity (high byte: integer, low byte: decimal)
 */
void App_ProcessDHT11(void)
{
    // C51 requirement: All variable declarations must be at function start
    static unsigned long last_dht11_tick = 0;
    unsigned char result;
    unsigned short temp_value, humidity_value;
    unsigned char temp_int, temp_dec_raw, temp_dec;
    unsigned char is_negative;
    unsigned char humidity_int, humidity_dec_raw, humidity_dec;
    
    // Check if 2.5 seconds have elapsed (non-blocking)
    if(Systick_Elapsed(last_dht11_tick, 2500))
    {
        // Read DHT11 data
        result = DHT11_Read_Data();
        
        if(result == 0)  // Read success
        {
            // Format temperature according to DHT11 protocol:
            // T_H (dht11_data[2]): Temperature integer part
            // T_L (dht11_data[3]): Temperature decimal part (0-9 for 0.0-0.9), Bit7 = 1表示负温度
            // 根据DHT11协议：小数部分为0-9（0x00-0x09），表示0.0-0.9
            temp_int = dht11_data[2];
            temp_dec_raw = dht11_data[3];
            temp_dec = 0;
            is_negative = 0;
            
            if(temp_dec_raw & 0x80)  // 检查温度低位的Bit7（最高位）是否为1（负温度）
            {
                // 负温度：清除符号位，小数部分为低7位（实际应该是0-9）
                is_negative = 1;
                temp_dec = temp_dec_raw & 0x0F;  // 只取低4位（0-9），忽略Bit7符号位和Bit4-6
                if(temp_dec > 9) temp_dec = 9;  // 限制在0-9范围内
            }
            else  // 正温度
            {
                // 正温度：小数部分为0-9
                temp_dec = temp_dec_raw & 0x0F;  // 只取低4位（0-9）
                if(temp_dec > 9) temp_dec = 9;  // 限制在0-9范围内
            }
            
            // 组合温度值：高字节=整数部分，低字节=小数部分（0-9）
            temp_value = ((unsigned short)temp_int << 8) | temp_dec;
            if(is_negative)
            {
                // 负温度：使用16位补码表示（最高位为1）
                if(temp_int == 0 && temp_dec == 0)
                {
                    temp_value = 0;  // 0度不是负数
                }
                else
                {
                    temp_value = 0x8000 | temp_value;  // 设置符号位
                }
            }
            
            // Format humidity: high byte = integer, low byte = decimal (0-9 for 0.0-0.9)
            // 根据DHT11协议：湿度小数部分为0-9（0x00-0x09），表示0.0-0.9
            humidity_int = dht11_data[0];
            humidity_dec_raw = dht11_data[1];
            humidity_dec = humidity_dec_raw & 0x0F;  // 只取低4位（0-9）
            if(humidity_dec > 9) humidity_dec = 9;  // 限制在0-9范围内
            
            humidity_value = ((unsigned short)humidity_int << 8) | humidity_dec;
            
            // Update MODBUS registers (3001=Temperature, 3002=Humidity)
            // Note: MODBUS register addresses are 0-based in code, so 3001 = index 3001
            // Data format: High byte = integer part, Low byte = decimal part (0-9 represents 0.0-0.9)
            // According to DHT11 protocol: decimal part is 0-9 (0x00-0x09), not 0-99
            // According to spec: "如果当次更新数据错误，则不刷新更新数据和显示数据"
            // "不论读取数据是否正确，自动读取时间间隔不变"
            Modbus_SetHoldingReg(3001, temp_value);      // Temperature: (temp_int << 8) | temp_dec (0-9)
            Modbus_SetHoldingReg(3002, humidity_value);  // Humidity: (humidity_int << 8) | humidity_dec (0-9)
            
            // DHT11 data updated to MODBUS registers, LCD will display it
        }
        // If read failed, don't update registers (keep last valid value)
        // According to spec: "不论读取数据是否正确，自动读取时间间隔不变" - time interval is already handled above
        
        // Update timestamp
        last_dht11_tick = Systick_GetTick();
    }
}

/**
 * Process automatic measurement (non-blocking)
 * Automatically triggers laser measurement at specified interval
 * Uses register 4001 (interval) and 4002 (countdown)
 * Simplified logic: core three steps - initialization, countdown update, trigger measurement
 * @param now: Current system tick from Systick_GetTick()
 */
void App_ProcessAutoMeasure(unsigned long now)
{
    // C51 requirement: All variable declarations must be at function start
    static unsigned long last_tick = 0;  // Last countdown update time
    static unsigned short countdown_reg = 0;  // Current countdown value in seconds
    static unsigned short last_countdown_reg_value = 0xFFFF;  // Last countdown register value to detect manual writes
    static unsigned long first_init_time = 0;  // First initialization time (for startup delay)
    
    unsigned short interval_reg;  // Current interval register value (4001)
    unsigned char interval_min, interval_sec;  // Parsed interval minutes and seconds
    unsigned long interval_total_sec;  // Total interval in seconds
    unsigned short manual_val;  // Manual write value from 4002
    unsigned char manual_min, manual_sec;  // Parsed manual minutes and seconds
    unsigned long manual_total_sec;  // Total manual seconds
    unsigned char countdown_min, countdown_sec;  // Countdown minutes and seconds for register format
    unsigned short countdown_value;  // Countdown value to write to register (format: min<<8 | sec)
    
    // 1. Initialize: Read interval from 4001
    interval_reg = Modbus_GetHoldingReg(4001);
    interval_min = (interval_reg >> 8) & 0xFF;
    interval_sec = interval_reg & 0xFF;
    interval_total_sec = (unsigned long)interval_min * 60 + (unsigned long)interval_sec;
    
    // If interval == 0, auto measure disabled: set 4002 to 0 and exit
    if(interval_total_sec == 0)
    {
        Modbus_SetHoldingReg(4002, 0);
        countdown_reg = 0;
        last_countdown_reg_value = 0;
        return;
    }
    
    // Initialize countdown if not initialized
    if(countdown_reg == 0 && last_countdown_reg_value == 0xFFFF)
    {
        // Record first initialization time
        if(first_init_time == 0)
        {
            first_init_time = now;
        }
        
        // Wait 5 seconds after system startup before starting auto measure
        // This gives laser module time to initialize and prevents 0xEA error on startup
        if((now - first_init_time) < 5000)
        {
            return;  // Skip initialization, wait for laser module to be ready
        }
        
        countdown_reg = interval_total_sec;
        countdown_min = countdown_reg / 60;
        countdown_sec = countdown_reg % 60;
        countdown_value = ((unsigned short)countdown_min << 8) | countdown_sec;
        Modbus_SetHoldingReg(4002, countdown_value);
        last_countdown_reg_value = countdown_value;
        last_tick = now;
    }
    
    // 2. Check for manual write to 4002
    manual_val = Modbus_GetHoldingReg(4002);
    if(manual_val != last_countdown_reg_value)
    {
        // Manual write detected
        manual_min = (manual_val >> 8) & 0xFF;
        manual_sec = manual_val & 0xFF;
        manual_total_sec = (unsigned long)manual_min * 60 + (unsigned long)manual_sec;
        
        if(manual_total_sec == 0)
        {
            // Write 0: trigger immediate measurement and reset countdown
            CmdQueue_Push("<mAm>", 0xFF, 5000);
            countdown_reg = interval_total_sec;  // Reset to interval
            countdown_min = countdown_reg / 60;
            countdown_sec = countdown_reg % 60;
            countdown_value = ((unsigned short)countdown_min << 8) | countdown_sec;
            Modbus_SetHoldingReg(4002, countdown_value);
            last_countdown_reg_value = countdown_value;
            last_tick = now;
        }
        else
        {
            // Write >0: update countdown from new value
            countdown_reg = manual_total_sec;
            last_countdown_reg_value = manual_val;
            last_tick = now;
        }
    }
    
    // 3. Countdown update: decrement every second
    if(interval_total_sec > 0)
    {
        if(Systick_Elapsed(last_tick, 1000))
        {
            last_tick += 1000;
            if(countdown_reg > 0)
            {
                countdown_reg--;
            }
            
            // Update register 4002 (format: high byte = minutes, low byte = seconds)
            countdown_min = countdown_reg / 60;
            countdown_sec = countdown_reg % 60;
            countdown_value = ((unsigned short)countdown_min << 8) | countdown_sec;
            Modbus_SetHoldingReg(4002, countdown_value);
            last_countdown_reg_value = countdown_value;
        }
    }
    
    // 4. Trigger measurement: when countdown reaches 0
    if(countdown_reg == 0 && interval_total_sec > 0)
    {
        // Trigger measurement
        CmdQueue_Push("<mAm>", 0xFF, 5000);
        
        // Reset countdown to interval
        countdown_reg = interval_total_sec;
        countdown_min = countdown_reg / 60;
        countdown_sec = countdown_reg % 60;
        countdown_value = ((unsigned short)countdown_min << 8) | countdown_sec;
        Modbus_SetHoldingReg(4002, countdown_value);
        last_countdown_reg_value = countdown_value;
        last_tick = now;
    }
}

/**
 * Process 4-20mA current output (non-blocking)
 * Reads level percentage from MODBUS register 3005 and outputs
 * corresponding 4-20mA current via GP8202AS DAC (I2C).
 * Update interval: 500ms
 * Mapping: 0% = 4mA (DAC=0x000), 100% = 20mA (DAC=0xFFF)
 */
void App_ProcessCurrentOutput(void)
{
    /* C51 requirement: All variable declarations must be at function start */
    static unsigned long last_current_output_tick = 0;
    unsigned short level_percent_reg;
    unsigned char level_percent_val;
    
    /* Non-blocking: update every 500ms */
    if(Systick_Elapsed(last_current_output_tick, 500))
    {
        /* Read level percentage from MODBUS register 3005 (BFB: 0-100) */
        level_percent_reg = Modbus_GetHoldingReg(3005);
        
        /* Clamp to 0-100 range */
        if(level_percent_reg > 100)
        {
            level_percent_val = 100;
        }
        else
        {
            level_percent_val = (unsigned char)level_percent_reg;
        }
        
        /* Output 4-20mA current via GP8202AS DAC */
        GP8202_SetPercent(level_percent_val);
        
        /* Update timestamp */
        last_current_output_tick = Systick_GetTick();
    }
}

/**
 * Draw Page 2: Company Information
 * Displays company info, product name, developer, contact and version
 */
static void App_DrawPage2(void)
{
    int y_pos;
    
    // Clear screen with background color
    LCM_Fill_Color(UI_BG_COLOR);
    
    // 第1行：广东丰穗米业 (32x32字体, y=15)
    // 6个字，每个32px宽，总宽192px，居中X=(240-192)/2=24px
    y_pos = 15;
    LCM_Draw_Char_Generic(24, y_pos, Font_Get32x32_Chinese("广"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(56, y_pos, Font_Get32x32_Chinese("东"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(88, y_pos, Font_Get32x32_Chinese("丰"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(120, y_pos, Font_Get32x32_Chinese("穗"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(152, y_pos, Font_Get32x32_Chinese("米"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(184, y_pos, Font_Get32x32_Chinese("业"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    
    // 第2行：产品：料位仪 (y=65)
    // 6个字，总宽192px，居中X=24px
    y_pos = 65;
    LCM_Draw_Char_Generic(24, y_pos, Font_Get32x32_Chinese("产"), 32, 32, UI_STATUS_LABEL, UI_BG_COLOR);
    LCM_Draw_Char_Generic(56, y_pos, Font_Get32x32_Chinese("品"), 32, 32, UI_STATUS_LABEL, UI_BG_COLOR);
    LCM_Draw_Char_Generic(88, y_pos, Font_Get32x32_Chinese("："), 32, 32, UI_STATUS_LABEL, UI_BG_COLOR);
    LCM_Draw_Char_Generic(120, y_pos, Font_Get32x32_Chinese("料"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(152, y_pos, Font_Get32x32_Chinese("位"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(184, y_pos, Font_Get32x32_Chinese("仪"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    
    // 第3行：开发：谢国良 (y=115)
    // 6个字，总宽192px，居中X=24px
    y_pos = 115;
    LCM_Draw_Char_Generic(24, y_pos, Font_Get32x32_Chinese("开"), 32, 32, UI_STATUS_LABEL, UI_BG_COLOR);
    LCM_Draw_Char_Generic(56, y_pos, Font_Get32x32_Chinese("发"), 32, 32, UI_STATUS_LABEL, UI_BG_COLOR);
    LCM_Draw_Char_Generic(88, y_pos, Font_Get32x32_Chinese("："), 32, 32, UI_STATUS_LABEL, UI_BG_COLOR);
    LCM_Draw_Char_Generic(120, y_pos, Font_Get32x32_Chinese("谢"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(152, y_pos, Font_Get32x32_Chinese("国"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    LCM_Draw_Char_Generic(184, y_pos, Font_Get32x32_Chinese("良"), 32, 32, UI_SENSOR_VALUE, UI_BG_COLOR);
    
    // 第4行：18022997999 (居中显示, y=165)
    // 11位数字，16x32字体，每位约16px宽，总宽约176px，居中X=(240-176)/2=32px
    y_pos = 165;
    LCM_Draw_String_16x32(32, y_pos, "18022997999", UI_SENSOR_VALUE, UI_BG_COLOR);
    
    // 第5行：V 1.0 (y=203, 使用16x32字体, 居中)
    // 5个字符（含空格和点），约80px宽，居中X=(240-80)/2=80px
    y_pos = 203;
    LCM_Draw_String_16x32(80, y_pos, "V 1.0", UI_SENSOR_VALUE, UI_BG_COLOR);
}

/**
 * Process LCD display update (non-blocking)
 * Updates LCD display with current sensor data
 * Currently displays temperature and humidity
 */
void App_ProcessLCD(void)
{
    // C51 requirement: All variable declarations must be at function start
    static unsigned long last_lcd_update_tick = 0;
    static unsigned char lcd_initialized = 0;
    // Static variables for MODBUS data
    static unsigned short temp_reg, humidity_reg;
    static unsigned char temp_int, temp_dec, humidity_int, humidity_dec;
    static unsigned short level_reg, addr_reg, start_reg, end_reg;  // Additional MODBUS registers
    static unsigned char hum_percent, level_percent;  // Bar graph percentages
    static unsigned char level_percent_invalid = 0;  // 1=invalid level percentage
    static unsigned char screen_timeout_reg = 0;  // Screen timeout value from register 4000
    // Note: screen_blackout is now a file-scope variable (app_screen_blackout) to share state with key handler
    // Screen timeout cache variables
    static unsigned long last_timeout_check_tick = 0;
    static unsigned char cached_timeout_reg = 0;
    static unsigned long last_activity_tick_check = 0;  // Track last activity tick to detect new activity
    // Temporary variables for calculations
    unsigned long timeout_ms, elapsed_activity_ms, remaining_screen_ms;
    unsigned char remaining_screen_sec;
    unsigned long lcd_refresh_interval;  // Dynamic LCD refresh interval
    // Variables for UI display
    float temp_value;
    unsigned long full_height, real_height;
    // Error code display variables
    unsigned short error_reg;
    const char* error_msg;
    char error_buf[16];
    unsigned char high_nibble, low_nibble;
    
    // Initialize LCD on first call
    if(lcd_initialized == 0)
    {
        // Configure P3.4 as push-pull output for backlight
        P3M0 |= 0x10;
        P3M1 &= ~0x10;
        
        // Turn on backlight (TP8006 requires >2.5V on DIM pin)
        LCM_PWM = 1;
        
        // Initialize LCD
        LCM_Init();
        Delay_ms(100);
        
        // Initialize UI system (will clear screen and draw status bar)
        UI_Init();
        Delay_ms(100);
        
        lcd_initialized = 1;
        last_lcd_update_tick = Systick_GetTick();
        app_last_activity_tick = Systick_GetTick();  // Initialize activity timer
        last_activity_tick_check = app_last_activity_tick;  // Initialize activity check
    }
    
    // Check info page timeout (1 minute auto-exit)
    if(info_page_active && Systick_Elapsed(info_page_start_tick, INFO_PAGE_TIMEOUT_MS))
    {
        // Info page timeout: exit to normal work page
        info_page_active = 0;
        current_page = 0;
        UI_Init();
        UI_ResetCache();
    }
    
    // Check setting state timeout (2 minutes auto-exit)
    if(setting_state != SETTING_STATE_IDLE && 
       Systick_Elapsed(last_setting_activity_tick, SETTING_TIMEOUT_MS))
    {
        // Setting state timeout: exit and save parameters
        App_ExitSettingState();
    }
    
    // Process screen timeout (non-blocking check on every call)
    // Cache timeout register value to avoid repeated MODBUS reads
    // Update cached timeout value every 100ms to reduce MODBUS register reads
    if(Systick_Elapsed(last_timeout_check_tick, 100))
    {
        cached_timeout_reg = Modbus_GetHoldingReg(4000) & 0xFF;
        last_timeout_check_tick = Systick_GetTick();
    }
    
    screen_timeout_reg = cached_timeout_reg;
    
    // Check screen timeout: if timeout_reg == 255, screen always on
    if(screen_timeout_reg != 255 && screen_timeout_reg > 0)
    {
        timeout_ms = (unsigned long)screen_timeout_reg * 1000;  // Convert seconds to milliseconds
        
        // Calculate screen timeout countdown
        if(Systick_GetTick() >= app_last_activity_tick)
        {
            elapsed_activity_ms = Systick_GetTick() - app_last_activity_tick;
        }
        else
        {
            elapsed_activity_ms = (0xFFFFFFFFUL - app_last_activity_tick) + Systick_GetTick();
        }
        
        if(elapsed_activity_ms < timeout_ms)
        {
            remaining_screen_ms = timeout_ms - elapsed_activity_ms;
            remaining_screen_sec = (unsigned char)((remaining_screen_ms + 999) / 1000);
        }
        else
        {
            remaining_screen_sec = 0;
        }
        
        // Check timeout: only use Systick_Elapsed to avoid false triggers
        if(Systick_Elapsed(app_last_activity_tick, timeout_ms))
        {
            // Timeout: clear screen, turn off backlight, and stop refreshing
            if(app_screen_blackout == 0)
            {
                app_screen_blackout = 1;
                
                // Clear screen to full black
                LCM_Fill_Color(RGB565_BLACK);
                // Turn off backlight to save power
                LCM_PWM = 0;
            }
            // Keep screen blackout state and stop all refresh until activity detected
            // LCD refresh is skipped when app_screen_blackout == 1 (see below)
        }
        else
        {
            // Not timeout: check if there was new activity (activity tick changed)
            // If activity tick changed, it means App_ResetActivityTimer() was called (new activity)
            if(app_last_activity_tick != last_activity_tick_check)
            {
                // New activity detected: turn on backlight and refresh UI
                if(app_screen_blackout == 1)
                {
                    app_screen_blackout = 0;
                    
                    // Turn on backlight
                    LCM_PWM = 1;
                    
                    // Restore current page display
                    if(current_page == 0)
                    {
                        // Page 1: Re-initialize sensor data UI and reset cache
                        UI_Init();
                        UI_ResetCache();  // 强制刷新所有数据
                    }
                    else
                    {
                        // Page 2: Redraw contact info
                        App_DrawPage2();
                    }
                }
                last_activity_tick_check = app_last_activity_tick;
            }
        }
    }
    else if(screen_timeout_reg == 255)
    {
        // Always on: ensure backlight is on and UI is displayed
        if(app_screen_blackout == 1)
        {
            app_screen_blackout = 0;
            // Turn on backlight
            LCM_PWM = 1;
            
            // Restore current page display
            if(current_page == 0)
            {
                // Page 1: Re-initialize sensor data UI and reset cache
                UI_Init();
                UI_ResetCache();  // 强制刷新所有数据
            }
            else
            {
                // Page 2: Redraw contact info
                App_DrawPage2();
            }
        }
    }
    
    // Non-blocking LCD refresh using new UI system
    // Dynamic refresh rate: ultra-smooth in setting state (30ms), normal otherwise (500ms)
    // Skip LCD refresh if screen is blacked out (screen timeout) to save resources
    lcd_refresh_interval = (setting_state != SETTING_STATE_IDLE) ? 30 : 500;
    if(app_screen_blackout == 0 && Systick_Elapsed(last_lcd_update_tick, lcd_refresh_interval))
    {
        // Check which page to display
        if(current_page == 0)
        {
        // Page 1: Sensor data (original UI)
        // Read current values from MODBUS registers
        temp_reg = Modbus_GetHoldingReg(3001);
        humidity_reg = Modbus_GetHoldingReg(3002);
        level_reg = Modbus_GetHoldingReg(3004);
        addr_reg = Modbus_GetHoldingReg(3000);
        start_reg = Modbus_GetHoldingReg(4003);
        end_reg = Modbus_GetHoldingReg(4004);
        
        // Read error code register (3006)
        error_reg = Modbus_GetHoldingReg(3006);
        error_msg = NULL;
        
        // Check error code and prepare error message for ST display
        if(error_reg != 0x00)
        {
            // Format error code as "Err:0xXX"
            error_buf[0] = 'E';
            error_buf[1] = 'r';
            error_buf[2] = 'r';
            error_buf[3] = ':';
            error_buf[4] = '0';
            error_buf[5] = 'x';
            
            // Convert error code to hex string
            high_nibble = (error_reg >> 4) & 0x0F;
            low_nibble = error_reg & 0x0F;
            error_buf[6] = (high_nibble < 10) ? ('0' + high_nibble) : ('A' + high_nibble - 10);
            error_buf[7] = (low_nibble < 10) ? ('0' + low_nibble) : ('A' + low_nibble - 10);
            error_buf[8] = '\0';
            
            error_msg = error_buf;
        }
        // If error_reg == 0x00, error_msg remains NULL (will display "Normal")
        
        // Extract temperature (handle sign bit)
        // Data format: High byte = integer, Low byte = decimal (0-9 for 0.0-0.9)
        if(temp_reg & 0x8000)  // Negative
        {
            temp_int = (temp_reg >> 8) & 0x7F;
            temp_dec = temp_reg & 0x0F;  // Only low 4 bits (0-9)
            if(temp_dec > 9) temp_dec = 9;
            temp_value = -(float)temp_int - (float)temp_dec * 0.1f;
        }
        else  // Positive
        {
            temp_int = (temp_reg >> 8) & 0x7F;
            temp_dec = temp_reg & 0x0F;  // Only low 4 bits (0-9)
            if(temp_dec > 9) temp_dec = 9;
            temp_value = (float)temp_int + (float)temp_dec * 0.1f;
        }
        
        // Extract humidity
        // Data format: High byte = integer, Low byte = decimal (0-9 for 0.0-0.9)
        humidity_int = (humidity_reg >> 8) & 0xFF;
        humidity_dec = humidity_reg & 0x0F;  // Only low 4 bits (0-9)
        if(humidity_dec > 9) humidity_dec = 9;
        hum_percent = humidity_int;
        if(hum_percent > 100) hum_percent = 100;
        
        // Calculate level percentage
        // 物理布局：
        // - start_reg: 起点位置（容器底部），传感器到底部的距离，如2000mm
        // - end_reg: 终点位置（有效测量终点），如1000mm
        // - level_reg: 激光测距（传感器到物料表面的距离），如1500mm
        // 料位计算：
        // - 有效范围 = start_reg - end_reg
        // - 物料高度 = start_reg - level_reg（从底部到物料表面）
        // - 料位百分比 = (物料高度 / 有效范围) * 100
        level_percent_invalid = 0;
        if(start_reg > end_reg)
        {
            full_height = (unsigned long)start_reg - (unsigned long)end_reg;
            
            // Update MODBUS register 3003 (PPS: Effective range = start - end)
            Modbus_SetHoldingReg(3003, (unsigned short)full_height);
            
            // 物料高度 = 传感器到底部的距离 - 传感器到物料表面的距离
            real_height = (unsigned long)start_reg - (unsigned long)level_reg;
            
            if(full_height > 0)
            {
                if(real_height > full_height)
                {
                    level_percent_invalid = 1;
                }
                level_percent = (unsigned char)((real_height * 100) / full_height);
                if(level_percent > 100) level_percent = 100;
            }
            else
            {
                level_percent = 0;
                level_percent_invalid = 1;
            }
        }
        else
        {
            // Invalid: start <= end, set PPS to 0
            Modbus_SetHoldingReg(3003, 0);
            level_percent = 0;
            level_percent_invalid = 1;
        }
        
        // Update MODBUS register 3005 (BFB: Level percentage 0-100)
        Modbus_SetHoldingReg(3005, level_percent);
        
        // Determine display parameters based on setting state
        {
            unsigned char start_setting_flag = 0;
            unsigned char end_setting_flag = 0;
            unsigned char addr_setting_flag = 0;
            int display_start = start_reg;
            int display_end = end_reg;  // 终点值（P行），正常模式显示end_reg
            int display_laser = level_reg;  // 激光测距位置（PS行）
            unsigned int display_addr = addr_reg;
            
            // 如果在设置状态，使用临时值显示
            if(setting_state == SETTING_STATE_START_MANUAL || setting_state == SETTING_STATE_START_LASER)
            {
                start_setting_flag = 1;
                display_start = temp_start_point;
            }
            else if(setting_state == SETTING_STATE_END_MANUAL || setting_state == SETTING_STATE_END_LASER)
            {
                end_setting_flag = 1;
                display_end = temp_end_point;  // 编辑模式显示临时终点值
            }
            else if(setting_state == SETTING_STATE_ADDR_MANUAL)
            {
                addr_setting_flag = 1;
                display_addr = temp_addr;
            }
            
            // 设置终点值（P行显示）
            UI_SetEndpoint(display_end);
            
            // Update UI with all data items (使用扩展版本支持设置状态显示)
            UI_Update_Ex(
                temp_value,        // 温度(℃)
                hum_percent,      // 湿度(%)
                display_laser,    // 激光测距位置PS(mm)
                level_percent,    // 料位百分比(%)
                display_start,    // 传感器高度S(mm) - 起点值（设置状态下显示临时值）
                display_addr,     // 地址（设置状态下显示临时值）
                error_msg,        // 错误报文（显示错误代码或Normal）
                start_setting_flag,  // 起点设置状态标志
                end_setting_flag,    // 终点设置状态标志
                addr_setting_flag    // 地址设置状态标志
            );
        }
        }
        else if(current_page == 1)
        {
            // Page 2: Contact information (no refresh needed, static display)
            // Do nothing, page content is already drawn when switching to this page
        }
        
        last_lcd_update_tick = Systick_GetTick();
    }
}

/**
 * Main application loop processing function
 * Processes all non-blocking tasks:
 * - MODBUS RTU requests (UART3)
 * - Key events
 * - Command queue
 * - UART4 responses
 * - DHT11 sensor reading
 * - LCD display update
 * - UART batch sending
 */
void App_MainLoop(void)
{
    unsigned long now = Systick_GetTick();
    KeyEventResult kev;  // C51 requirement: variable declarations at function start
    static unsigned long last_debug_tick = 0;  // for UART0 debug heartbeat

    // Debug: UART0 heartbeat every 2s (9600 8N1, connect to see if main loop runs)
    if(Systick_Elapsed(last_debug_tick, 2000))
    {
        last_debug_tick = now;
        UART0_QueueString("[DBG] ok\r\n");
    }

    // HIGH PRIORITY: Process key events FIRST (non-blocking FSM, no delays)
    // This ensures immediate key response even if LCD refresh is in progress
    {
        Key_Process(now, &kev);
        if(kev.event != KEY_NONE && kev.key_id < KEY_COUNT)
        {
            // === K1键快捷操作 (key_id == 0) ===
            if(kev.key_id == 0)
            {
                if(kev.event == KEY_SINGLE)
                {
                    // K1单击操作
                    if(app_screen_blackout == 1)
                    {
                        // 熄屏状态下单击：只点亮屏幕
                        App_ResetActivityTimer();
                        // 屏幕将在App_ProcessLCD中被点亮
                    }
                    else if(info_page_active)
                    {
                        // 信息页面显示时单击：手动退出信息页面，返回正常工作页面
                        info_page_active = 0;
                        current_page = 0;
                        UI_Init();
                        UI_ResetCache();
                        App_ResetActivityTimer();
                    }
                    else
                    {
                        // 亮屏状态下单击：只重置活动定时器（延长屏幕点亮时间）
                        App_ResetActivityTimer();
                    }
                }
                else if(kev.event == KEY_DOUBLE)
                {
                    // K1双击操作：设置状态下=激光标定，正常状态=立即测量料位
                    if(app_screen_blackout == 1)
                    {
                        // 熄屏状态下双击：先点亮屏幕，再启动测量
                        App_ResetActivityTimer();
                        // 延迟一小段时间确保屏幕点亮后再发送测量命令
                        Delay_ms(50);
                    }
                    
                    // 判断当前状态：设置状态 vs 正常状态
                    if(setting_state == SETTING_STATE_START_MANUAL || 
                       setting_state == SETTING_STATE_END_MANUAL ||
                       setting_state == SETTING_STATE_START_LASER || 
                       setting_state == SETTING_STATE_END_LASER)
                    {
                        // 设置状态：启动激光标定（测距值填入临时参数，可继续手动调整）
                        CmdQueue_Push("<mAm>", 5, 5000);  // key_id=5触发激光标定回调
                        setting_modified = 1;
                        App_UpdateSettingActivity();
                    }
                    else
                    {
                        // 正常状态：启动立即测量料位
                        CmdQueue_Push("<mAm>", 0, 5000);  // key_id=0正常测量
                    }
                    App_ResetActivityTimer();
                }
                else if(kev.event == KEY_LONG_PRESS)
                {
                    // K1长按操作：显示信息页面
                    if(app_screen_blackout == 1)
                    {
                        // 熄屏状态下长按：先点亮屏幕，再显示信息页面
                        App_ResetActivityTimer();
                        // 延迟一小段时间确保屏幕点亮后再切换页面
                        Delay_ms(50);
                    }
                    // 显示信息页面
                    info_page_active = 1;
                    current_page = 1;
                    App_DrawPage2();
                    info_page_start_tick = Systick_GetTick();
                    App_ResetActivityTimer();
                }
            }
            else
            {
                // === K2/K3/K5/K6按键处理：料位起止点设置 ===
                // Key activity: reset activity timer (this will also turn on backlight in App_ProcessLCD)
                App_ResetActivityTimer();
                
                // K2键：起点设置
                if(kev.key_id == 1)  // KEY2 = key_id 1
                {
                    if(kev.event == KEY_SINGLE)
                    {
                        if(setting_state == SETTING_STATE_START_MANUAL || setting_state == SETTING_STATE_START_LASER)
                        {
                            // 起点设置状态下单击K2：手动退出并保存
                            App_ExitSettingState();
                        }
                        else if(setting_state == SETTING_STATE_IDLE)
                        {
                            // 正常状态下单击K2：进入起点手动设置状态
                            App_EnterSettingState(SETTING_STATE_START_MANUAL);
                        }
                    }
                    else if(kev.event == KEY_LONG_PRESS)
                    {
                        if(setting_state == SETTING_STATE_IDLE)
                        {
                            // 正常状态下长按K2：进入起点激光标定状态
                            App_EnterSettingState(SETTING_STATE_START_LASER);
                        }
                    }
                }
                // K3键：终点设置
                else if(kev.key_id == 2)  // KEY3 = key_id 2
                {
                    if(kev.event == KEY_SINGLE)
                    {
                        if(setting_state == SETTING_STATE_END_MANUAL || setting_state == SETTING_STATE_END_LASER)
                        {
                            // 终点设置状态下单击K3：手动退出并保存
                            App_ExitSettingState();
                        }
                        else if(setting_state == SETTING_STATE_IDLE)
                        {
                            // 正常状态下单击K3：进入终点手动设置状态
                            App_EnterSettingState(SETTING_STATE_END_MANUAL);
                        }
                    }
                    else if(kev.event == KEY_LONG_PRESS)
                    {
                        if(setting_state == SETTING_STATE_IDLE)
                        {
                            // 正常状态下长按K3：进入终点激光标定状态
                            App_EnterSettingState(SETTING_STATE_END_LASER);
                        }
                    }
                }
                // K4键：地址设置
                else if(kev.key_id == 3)  // KEY4 = key_id 3
                {
                    if(kev.event == KEY_SINGLE)
                    {
                        if(setting_state == SETTING_STATE_ADDR_MANUAL)
                        {
                            // 地址设置状态下单击K4：手动退出并保存
                            App_ExitSettingState();
                        }
                        else if(setting_state == SETTING_STATE_IDLE)
                        {
                            // 正常状态下单击K4：进入地址手动设置状态
                            App_EnterSettingState(SETTING_STATE_ADDR_MANUAL);
                        }
                    }
                    // K4不支持长按进入设置（只支持单击）
                }
                // K5键：递减 / 激光标定
                else if(kev.key_id == 4)  // KEY5 = key_id 4
                {
                    if(kev.event == KEY_SINGLE)
                    {
                        // 根据当前设置状态执行不同操作
                        if(setting_state == SETTING_STATE_START_MANUAL)
                        {
                            // 起点手动设置状态：递减1
                            if(temp_start_point > 0)
                            {
                                temp_start_point--;
                                setting_modified = 1;
                                App_UpdateSettingActivity();
                            }
                        }
                        else if(setting_state == SETTING_STATE_END_MANUAL)
                        {
                            // 终点手动设置状态：递减1
                            if(temp_end_point > 0)
                            {
                                temp_end_point--;
                                setting_modified = 1;
                                App_UpdateSettingActivity();
                            }
                        }
                        else if(setting_state == SETTING_STATE_ADDR_MANUAL)
                        {
                            // 地址手动设置状态：递减1（1->247循环）
                            if(temp_addr > 1)
                            {
                                temp_addr--;
                            }
                            else
                            {
                                temp_addr = 247;  // 从1递减到247
                            }
                            setting_modified = 1;
                            App_UpdateSettingActivity();
                        }
                        else if(setting_state == SETTING_STATE_START_LASER)
                        {
                            // 起点激光标定状态：触发测距并标定
                            CmdQueue_Push("<mAm>", 5, 5000);
                            // 测量结果将通过cmd_queue回调更新temp_start_point
                            setting_modified = 1;
                            App_UpdateSettingActivity();
                        }
                        else if(setting_state == SETTING_STATE_END_LASER)
                        {
                            // 终点激光标定状态：触发测距并标定
                            CmdQueue_Push("<mAm>", 5, 5000);
                            // 测量结果将通过cmd_queue回调更新temp_end_point
                            setting_modified = 1;
                            App_UpdateSettingActivity();
                        }
                    }
                }
                // K6键：递加
                else if(kev.key_id == 5)  // KEY6 = key_id 5
                {
                    if(kev.event == KEY_SINGLE)
                    {
                        // 根据当前设置状态执行不同操作（仅手动设置模式）
                        if(setting_state == SETTING_STATE_START_MANUAL)
                        {
                            // 起点手动设置状态：递加1
                            if(temp_start_point < 65535)
                            {
                                temp_start_point++;
                                setting_modified = 1;
                                App_UpdateSettingActivity();
                            }
                        }
                        else if(setting_state == SETTING_STATE_END_MANUAL)
                        {
                            // 终点手动设置状态：递加1
                            if(temp_end_point < 65535)
                            {
                                temp_end_point++;
                                setting_modified = 1;
                                App_UpdateSettingActivity();
                            }
                        }
                        else if(setting_state == SETTING_STATE_ADDR_MANUAL)
                        {
                            // 地址手动设置状态：递加1（247->1循环）
                            if(temp_addr < 247)
                            {
                                temp_addr++;
                            }
                            else
                            {
                                temp_addr = 1;  // 从247递加到1
                            }
                            setting_modified = 1;
                            App_UpdateSettingActivity();
                        }
                    }
                }
                // 其他按键保持原有功能
                else
                {
                    // 其他按键暂无额外功能
                }
            }
        }
    }
    
    // Process command queue (non-blocking)
    CmdQueue_Process(now);
    
    // Process K5/K6 long press adjust (for parameter adjustment acceleration)
    App_ProcessLongPressAdjust(now);
    
    // Check for UART4 responses (for command queue)
    App_ProcessUart4Response();
    
    // CRITICAL FIX: Process MODBUS RTU aggressively (3 times per loop)
    // NE2 can send data faster than MCU processes, need to drain UART3 RX buffer
    Modbus_Process();
    Modbus_Process();
    Modbus_Process();  // Process 3 times to handle burst requests
    
    // Trigger UART send batches if needed (non-blocking)
    // CRITICAL: Move here to send MODBUS responses immediately after processing
    App_TriggerUartSending();
    
    // Process DHT11 sensor reading (non-blocking, updates MODBUS registers)
    App_ProcessDHT11();
    
    // Process automatic measurement (non-blocking)
    App_ProcessAutoMeasure(now);
    
    // Process 4-20mA current output (non-blocking, updates GP8202AS DAC)
    App_ProcessCurrentOutput();
    
    // LOW PRIORITY: Process LCD display update LAST (may take long time)
    // This ensures keys are processed even during LCD refresh
    App_ProcessLCD();
    
    // Trigger send again at the end of loop to ensure responses go out
    App_TriggerUartSending();
}

/**
 * Reset screen activity timer (called when key is pressed or data updated)
 * This will prevent screen timeout and turn on backlight if it was off
 */
void App_ResetActivityTimer(void)
{
    app_last_activity_tick = Systick_GetTick();
}

/**
 * Laser calibration callback (called by cmd_queue when K5 or K1 double-click triggers measurement)
 * @param level_value: Measured distance value in mm
 */
void App_LaserCalibrationCallback(unsigned short level_value)
{
    // 支持所有起点设置状态（手动+激光）
    if(setting_state == SETTING_STATE_START_LASER || 
       setting_state == SETTING_STATE_START_MANUAL)
    {
        // 起点标定：使用测量值作为新起点（可继续手动调整）
        temp_start_point = level_value;
        setting_modified = 1;
        App_UpdateSettingActivity();
    }
    // 支持所有终点设置状态（手动+激光）
    else if(setting_state == SETTING_STATE_END_LASER || 
            setting_state == SETTING_STATE_END_MANUAL)
    {
        // 终点标定：使用测量值作为新终点（可继续手动调整）
        temp_end_point = level_value;
        setting_modified = 1;
        App_UpdateSettingActivity();
    }
}

#if 0
/**
 * Notify that a measurement was successful
 * This will clear manual countdown mode and resume normal auto-measure interval
 * According to spec: "直到有一次正确的测量结果后再按照设置的自动测量间隔"
 * Note: Currently not used, cmd_queue.c directly sets auto_measure_manual_countdown_active_flag = 0
 */
void App_NotifyMeasurementSuccess(void)
{
    // Set flag to clear manual countdown mode
    auto_measure_manual_countdown_active_flag = 0;
}
#endif

#if 0
/**
 * Test function: Toggle backlight on/off
 * This is for testing backlight control
 * @param state: 1 = turn on, 0 = turn off
 */
void App_TestBacklight(unsigned char state)
{
    LCM_PWM = state ? 1 : 0;
}
#endif

