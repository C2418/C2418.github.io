#include "cmd_queue.h"
#include "uart.h"
#include "systick.h"
#include "key.h"
#include "modbus.h"
#include "app.h"  // For auto_measure_manual_countdown_active_flag
#include <string.h>

// Command queue
static CmdItem cmd_queue[CMD_QUEUE_SIZE];
static unsigned char q_head = 0;
static unsigned char q_tail = 0;
static unsigned char q_count = 0;

/**
 * Initialize command queue
 */
void CmdQueue_Init(void)
{
    unsigned char i;
    q_head = 0;
    q_tail = 0;
    q_count = 0;
    
    // Initialize all queue items
    for(i = 0; i < CMD_QUEUE_SIZE; i++)
    {
        cmd_queue[i].state = CMD_IDLE;
        cmd_queue[i].cmd[0] = '\0';
        cmd_queue[i].key_id = 0;
        cmd_queue[i].timeout_ms = 0;
        cmd_queue[i].sent_ts = 0;
    }
}

/**
 * Push a command to queue (non-blocking)
 * @param cmd: Command string to send
 * @param key_id: Key ID (0-5) that triggered this command
 * @param timeout_ms: Timeout in milliseconds
 * @return: 0 on success, 1 if queue is full
 */
unsigned char CmdQueue_Push(const char *cmd, unsigned char key_id, unsigned long timeout_ms)
{
    if(q_count >= CMD_QUEUE_SIZE)
    {
        return 1;  // Queue full
    }
    
    // Copy command string
    strncpy(cmd_queue[q_tail].cmd, cmd, sizeof(cmd_queue[q_tail].cmd) - 1);
    cmd_queue[q_tail].cmd[sizeof(cmd_queue[q_tail].cmd) - 1] = '\0';
    
    // Set command parameters
    cmd_queue[q_tail].key_id = key_id;
    cmd_queue[q_tail].timeout_ms = timeout_ms;
    cmd_queue[q_tail].state = CMD_IDLE;
    cmd_queue[q_tail].sent_ts = 0;
    
    // Update queue pointers
    q_tail = (q_tail + 1) % CMD_QUEUE_SIZE;
    q_count++;
    
    return 0;  // Success
}

/**
 * Process command queue (call in main loop, non-blocking)
 * @param now: Current tick (from Systick_GetTick())
 */
void CmdQueue_Process(unsigned long now)
{
    unsigned long elapsed;
    CmdItem *item;
    
    if(q_count == 0)
    {
        return;  // Queue empty
    }
    
    item = &cmd_queue[q_head];
    
    switch(item->state)
    {
        case CMD_IDLE:
            // Send command (non-blocking)
            if(Uart4_QueueString(item->cmd) == 0)
            {
                // Command queued successfully
                item->state = CMD_SENT;
            }
            else
            {
                // Failed to queue (buffer full), will retry next time
                // Keep state as CMD_IDLE
            }
            break;
            
        case CMD_SENT:
            // Ensure UART4 send is triggered (in case it wasn't triggered by QueueString)
            if(Uart4_IsSending() == 0 && Uart4_HasDataToSend())
            {
                Uart4_SendBatch();  // Trigger send if not already sending
            }
            // Wait for UART to finish sending before waiting for reply
            // Check if UART4 has finished sending (both sending flag and buffer are empty)
            if(Uart4_IsSending() == 0 && Uart4_HasDataToSend() == 0)
            {
                // Command sent completely, now wait for reply.
                // Start timeout measurement at the moment the last byte is out.
                item->sent_ts = now;
                item->state = CMD_WAIT_REPLY;
            }
            // If still sending, stay in CMD_SENT state and check again next time
            break;
            
        case CMD_WAIT_REPLY:
            // Check timeout
            elapsed = (now >= item->sent_ts) ? (now - item->sent_ts) : (0xFFFFFFFFUL - item->sent_ts + now + 1);
            if(elapsed >= item->timeout_ms)
            {
                // Timeout: set error code 0xEA (超时)
                // According to spec: "发送测距指令超过4秒仍然没有接收到测距模块反馈数据，则判断超时"
                Modbus_SetHoldingReg(3006, 0xEA);
                item->state = CMD_TIMEOUT;
            }
            break;
            
        case CMD_DONE:
        case CMD_TIMEOUT:
            // Command completed or timed out, remove from queue
            q_head = (q_head + 1) % CMD_QUEUE_SIZE;
            q_count--;
            break;
            
        default:
            break;
    }
}

/**
 * Convert hex character to value (0-15)
 * @param c: Hex character ('0'-'9', 'A'-'F', 'a'-'f')
 * @return: Value (0-15), or 0xFF if invalid
 */
#if 0
static unsigned char HexCharToValue(unsigned char c)
{
    if(c >= '0' && c <= '9')
    {
        return c - '0';
    }
    else if(c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    else if(c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return 0xFF;  // Invalid
}
#endif

/**
 * Parse string to unsigned short (decimal)
 * @param str: String to parse
 * @param len: Length of string
 * @param value: Pointer to store parsed value
 * @return: 0 on success, 1 on error
 */
static unsigned char ParseDecimal(const char *str, unsigned char len, unsigned short *value)
{
    unsigned char i;
    unsigned short result = 0;
    
    if(len == 0 || len > 5)  // Max 5 digits for unsigned short (65535)
    {
        return 1;  // Invalid length
    }
    
    for(i = 0; i < len; i++)
    {
        if(str[i] < '0' || str[i] > '9')
        {
            return 1;  // Invalid character
        }
        {
            unsigned char digit = str[i] - '0';
            if(result > (65535UL - digit) / 10UL) return 1;
            result = (unsigned short)(result * 10UL + digit);
        }
    }
    
    *value = result;
    return 0;  // Success
}

/**
 * Handle received response (call when UART4 receives data)
 * @param resp: Received response string
 */
void CmdQueue_HandleResponse(const char *resp)
{
    // C51 requirement: All variable declarations must be at function start
    CmdItem *item;
    unsigned char resp_len;
    unsigned char data_len;
    unsigned char expected_len;
    unsigned short level_value;
    unsigned short start_point;
    unsigned short end_point;
    unsigned char error_code_high;
    unsigned char error_code_low;
    unsigned char error_code;
    unsigned char percentage;
    
    if(q_count == 0)
    {
        return;  // No pending commands
    }
    
    // Check if response matches expected format (starts with '<')
    if(resp == 0 || resp[0] != '<')
    {
        return;  // Invalid response format
    }
    
    // Find first command waiting for reply
    item = &cmd_queue[q_head];
    if(item->state == CMD_WAIT_REPLY)
    {
        // Special handling for manual measure (key_id=0), KEY3 (key_id=2), auto measure (key_id=0xFF), and KEY5 calibration (key_id=5): response like <m40m3360>
        // Parse laser rangefinder response
        // Protocol format:
        //   Normal: <mX0mxxxxxx> where X is data length (1 digit, 0-9), xxxxxx is distance data (decimal)
        //   Example: <m30m500> = length 3, data 500 (0.5 meter)
        //   Example: <m40m1000> = length 4, data 1000 (1 meter)
        //   Error: <0Ezzz> where zzz is 3-digit error code
        if((item->key_id == 0 || item->key_id == 2 || item->key_id == 0xFF || item->key_id == 5) && resp[0] == '<')
        {
            resp_len = strlen(resp);
            
            // Check for error format: <0Ezzz> where zzz is 3-digit error code
            // Error codes: 0EAH (超时), 0EBH (通信错误), 0ECH (超过距离), 0EDH (大于起点), 0EEH (小于终点)
            if(resp_len >= 6 && resp[1] == '0' && resp[2] == 'E' && resp[resp_len - 1] == '>')
            {
                // Parse error code (3-digit hex: positions 3, 4, 5)
                error_code_high = resp[3];
                error_code_low = resp[4];
                error_code = 0;
                
                // Convert hex to value
                if(error_code_high >= '0' && error_code_high <= '9')
                    error_code = (error_code_high - '0') << 4;
                else if(error_code_high >= 'A' && error_code_high <= 'F')
                    error_code = (error_code_high - 'A' + 10) << 4;
                else if(error_code_high >= 'a' && error_code_high <= 'f')
                    error_code = (error_code_high - 'a' + 10) << 4;
                
                if(error_code_low >= '0' && error_code_low <= '9')
                    error_code |= (error_code_low - '0');
                else if(error_code_low >= 'A' && error_code_low <= 'F')
                    error_code |= (error_code_low - 'A' + 10);
                else if(error_code_low >= 'a' && error_code_low <= 'f')
                    error_code |= (error_code_low - 'a' + 10);
                
                // Set error code to register 3006
                Modbus_SetHoldingReg(3006, error_code);
                
                // According to spec: "如果测量错误，则不更新当前料位、料位余量、百分比等的计算和显示，也不更新远程读取参数的更新"
                // So we don't update register 3004, 3005, etc.
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Check normal format: <mX0mxxxxxx>
            // Format: <m + length(1 digit) + '0' + m + data + >
            // Minimum: <mX0m> = 5 chars, need at least <mX0mD> = 6 chars
            if(resp_len < 6 || resp[1] != 'm' || resp[resp_len - 1] != '>')
            {
                // Invalid format
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Extract length field (position 2, 1 digit: 0-9)
            if(resp[2] < '0' || resp[2] > '9')
            {
                // Invalid length digit
                item->state = CMD_TIMEOUT;
                return;
            }
            expected_len = resp[2] - '0';  // Convert ASCII to decimal
            
            // Check fixed '0' at position 3
            if(resp[3] != '0')
            {
                // Invalid format, missing '0'
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Check separator 'm' at position 4
            if(resp[4] != 'm')
            {
                // Invalid format, missing 'm' separator
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Calculate actual data length
            // Data starts at position 5 (after "<mX0m")
            // Data ends before '>' at position resp_len-1
            data_len = resp_len - 1 - 5;  // Total length - '<' - 'm' - 'X' - '0' - 'm' - '>'
            
            // Verify data length matches expected length
            if(data_len != expected_len)
            {
                // Length mismatch
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Parse data string to number (decimal, starting at position 5)
            if(ParseDecimal(&resp[5], data_len, &level_value) != 0)
            {
                // Parse error: set error code 0xEB (通信错误)
                Modbus_SetHoldingReg(3006, 0xEB);
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Validate measurement value according to spec:
            // - 0ECH: 超过距离 (measurement exceeds maximum limit)
            // - 0EDH: 大于起点 (measurement > start point)
            // - 0EEH: 小于终点 (measurement < end point)
            // Get start and end points from registers 4003 and 4004
            start_point = Modbus_GetHoldingReg(4003);
            end_point = Modbus_GetHoldingReg(4004);
            
            // Check if measurement > start point (0EDH error)
            if(level_value > start_point)
            {
                Modbus_SetHoldingReg(3006, 0xED);
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Check if measurement < end point (0EEH error)
            if(level_value < end_point)
            {
                Modbus_SetHoldingReg(3006, 0xEE);
                item->state = CMD_TIMEOUT;
                return;
            }
            
            // Use the same validated physical-height calculation as the display.
            if(item->key_id != 5)
            {
                if(!App_CalculateLevelPercent(start_point, end_point, level_value, &percentage))
                {
                    Modbus_SetHoldingReg(3006, 0xEE);
                    item->state = CMD_TIMEOUT;
                    return;
                }
            }

            // Commit registers only after all measurement validation succeeds.
            Modbus_SetHoldingReg(3006, 0x00);

            // KEY5 updates the temporary calibration value only.
            if(item->key_id == 5)
            {
                extern void App_LaserCalibrationCallback(unsigned short level_value);
                App_LaserCalibrationCallback(level_value);
            }
            else
            {
                Modbus_SetHoldingReg(3004, level_value);
                Modbus_SetHoldingReg(3005, percentage);
            }
            
            // Notify app layer that measurement was successful
            // According to spec: "直到有一次正确的测量结果后再按照设置的自动测量间隔"
            auto_measure_manual_countdown_active_flag = 0;  // Clear manual countdown mode
            
            item->state = CMD_DONE;
            
            // LCD will display the updated data automatically
        }
        // Default handling for other keys
        else
        {
            // Minimal validation: treat only "<OK" (or longer OK variants) as success
            if(strncmp(resp, "<OK", 3) == 0)
            {
                item->state = CMD_DONE;
            }
            else
            {
                item->state = CMD_TIMEOUT;  // Treat other responses as error/timeout
            }
            
            // Response processed, LCD will show updated data
        }
    }
}

/**
 * Check if queue has pending commands
 * NOTE: Not used in current project; kept under #if 0 to avoid L16 warning.
 * @return: 1 if queue has commands, 0 if empty
 */
#if 0
unsigned char CmdQueue_HasPending(void)
{
    return (q_count > 0) ? 1 : 0;
}
#endif

