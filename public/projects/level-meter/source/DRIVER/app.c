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
#include "delay.h"
#include "STC8H.h"  // For PWMA_PS register definition
#include "STC8G_PWM15bit.h"  // For PWMSET register and PWM15_PWM4_Set macro

// Global variable for screen activity timer (shared between App_ProcessLCD and App_MainLoop)
static unsigned long app_last_activity_tick = 0;

// Global flag to clear manual countdown mode when measurement succeeds
unsigned char auto_measure_manual_countdown_active_flag = 0;

// UART4 response buffer for command queue
static xdata char uart4_rx_buf[64];
static unsigned char uart4_rx_idx = 0;

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
    // Disabled: UART3 not used (MODBUS/NE2 disabled)
    // if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
    // {
    //     Uart3_SendBatch();
    // }
    
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
    static unsigned char screen_blackout = 0;  // Screen blackout state: 1=black screen (timeout), 0=normal display
    // Screen timeout cache variables
    static unsigned long last_timeout_check_tick = 0;
    static unsigned char cached_timeout_reg = 0;
    static unsigned long last_activity_tick_check = 0;  // Track last activity tick to detect new activity
    // Temporary variables for calculations
    unsigned long timeout_ms, elapsed_activity_ms, remaining_screen_ms;
    unsigned char remaining_screen_sec;
    // Variables for UI display
    float temp_value;
    unsigned char auto_mode;
    unsigned long full_height, real_height;
    
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
            // Timeout: clear screen and stop refreshing
            if(screen_blackout == 0)
            {
                screen_blackout = 1;
                
                // Clear entire screen with UI background color
                UI_Clear();
            }
            // Keep screen blackout state and stop all refresh until activity detected
            // LCD refresh is skipped when screen_blackout == 1 (see below)
        }
        else
        {
            // Not timeout: check if there was new activity (activity tick changed)
            // If activity tick changed, it means App_ResetActivityTimer() was called (new activity)
            if(app_last_activity_tick != last_activity_tick_check)
            {
                // New activity detected: clear blackout and resume normal display
                if(screen_blackout == 1)
                {
                    screen_blackout = 0;
                    
                    // Re-initialize UI to restore display
                    UI_Init();
                }
                last_activity_tick_check = app_last_activity_tick;
            }
        }
    }
    else if(screen_timeout_reg == 255)
    {
        // Always on: ensure screen is not blacked out
        if(screen_blackout == 1)
        {
            screen_blackout = 0;
            // Re-initialize UI to restore display
            UI_Init();
        }
    }
    
    // Non-blocking LCD refresh using new UI system
    // Update every 500ms to reduce CPU load while maintaining smooth display
    // Skip LCD refresh if screen is blacked out (screen timeout) to save resources
    if(screen_blackout == 0 && Systick_Elapsed(last_lcd_update_tick, 500))
    {
        // Read current values from MODBUS registers
        temp_reg = Modbus_GetHoldingReg(3001);
        humidity_reg = Modbus_GetHoldingReg(3002);
        level_reg = Modbus_GetHoldingReg(3004);
        addr_reg = Modbus_GetHoldingReg(3000);
        start_reg = Modbus_GetHoldingReg(4003);
        end_reg = Modbus_GetHoldingReg(4004);
        
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
        level_percent_invalid = 0;
        if(start_reg > end_reg)
        {
            full_height = (unsigned long)start_reg - (unsigned long)end_reg;
            
            // Update MODBUS register 3003 (PPS: Effective range = start - end)
            Modbus_SetHoldingReg(3003, (unsigned short)full_height);
            
            real_height = (unsigned long)start_reg - (unsigned long)level_reg - (unsigned long)end_reg;
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
        
        // Get auto mode from register (assuming register 4001 bit 0)
        auto_mode = (Modbus_GetHoldingReg(4001) & 0x01) ? 1 : 0;
        
        // Update UI with all 5 data items
        UI_Update(
            temp_value,        // 温度
            hum_percent,      // 湿度
            level_percent,    // 液位百分比
            addr_reg,         // 寄存器地址
            level_reg         // 液位当前值
        );
        
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
    
    // HIGH PRIORITY: Process key events FIRST (non-blocking FSM, no delays)
    // This ensures immediate key response even if LCD refresh is in progress
    {
        Key_Process(now, &kev);
        if(kev.event != KEY_NONE && kev.key_id < KEY_COUNT)
        {
            // Key activity: reset activity timer (this will also turn on backlight in App_ProcessLCD)
            App_ResetActivityTimer();
            
            switch(kev.event)
            {
                case KEY_SINGLE:
                    switch(kev.key_id)
                    {
                        case 0:
                            Laser_On();
                            break;
                        case 1:
                            Laser_Off();
                            break;
                        case 2:
                            // KEY3: Trigger laser measurement
                            CmdQueue_Push("<mAm>", 2, 5000);
                            break;
                        case 3:
                            CmdQueue_Push("<RDAD>", 3, 5000);
                            break;
                        case 4:
                            CmdQueue_Push("<STAD+0100>", 4, 5000);
                            break;
                        case 5:
                            CmdQueue_Push("<STAD-0100>", 5, 5000);
                            break;
                        default:
                            break;
                    }
                    break;

                case KEY_LONG_PRESS:
                    // Long press - no action needed, LCD will show updated data
                    break;

                default:
                    break;
            }
        }
    }
    
    // Process command queue (non-blocking)
    CmdQueue_Process(now);
    
    // Check for UART4 responses (for command queue)
    App_ProcessUart4Response();
    
    // Process MODBUS RTU frames from UART3 (non-blocking)
    Modbus_Process();
    
    // Process DHT11 sensor reading (non-blocking, updates MODBUS registers)
    App_ProcessDHT11();
    
    // Process automatic measurement (non-blocking)
    App_ProcessAutoMeasure(now);
    
    // LOW PRIORITY: Process LCD display update LAST (may take long time)
    // This ensures keys are processed even during LCD refresh
    App_ProcessLCD();
    
    // Trigger UART send batches if needed (non-blocking)
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

