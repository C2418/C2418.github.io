#include "modbus.h"
#include "uart.h"
#include "delay.h"
#include "systick.h"

// ========================================
// MODBUS RTU Slave Implementation
// ========================================
// MCU acts as Modbus-RTU slave, responding to requests from host
// NE2 module converts Modbus-TCP (from host) <-> Modbus-RTU (to MCU)

// MODBUS RTU Frame Format:
// [Slave Address] [Function Code] [Data...] [CRC16 Low] [CRC16 High]

// Internal register table (holding registers)
// Only store actual used registers to save XDATA space:
// - 3000-3006: 7 read-only registers (mapped to index 0-6)
// - 4000-4004: 5 read-write registers (mapped to index 7-11)
#define MODBUS_REG_COUNT 12
static unsigned short modbus_holding_regs[MODBUS_REG_COUNT];

// Register address mapping functions
// Map MODBUS address to internal array index
static unsigned short Modbus_AddrToIndex(unsigned short addr)
{
    if(addr >= 3000 && addr <= 3006)
    {
        return addr - 3000;  // 3000-3006 -> 0-6
    }
    else if(addr >= 4000 && addr <= 4004)
    {
        return 7 + (addr - 4000);  // 4000-4004 -> 7-11
    }
    return 0xFFFF;  // Invalid address
}

// Check if address is valid
static unsigned char Modbus_IsValidAddr(unsigned short addr)
{
    return (addr >= 3000 && addr <= 3006) || (addr >= 4000 && addr <= 4004);
}

// Receive buffer for MODBUS RTU frame
#define MODBUS_RX_BUF_SIZE 64
static unsigned char modbus_rx_buffer[MODBUS_RX_BUF_SIZE];
static unsigned char modbus_rx_index = 0;
static unsigned char modbus_frame_ready = 0;

// MODBUS slave address (1-247, 0 is broadcast)
static unsigned char modbus_slave_addr = 1;

// Frame timeout detection (3.5 character time)
// At 9600bps: 1 char = 1.04ms, 3.5 char ≈ 3.65ms
// We now base timeout on Systick_GetTick() instead of assuming a fixed
// Modbus_Process() call frequency.
#define MODBUS_FRAME_TIMEOUT_MS 4  // ≈3.5 character times at 9600bps
static unsigned long modbus_last_rx_tick = 0;

/**
 * Calculate MODBUS CRC16
 * @param buf: Pointer to data buffer
 * @param length: Data length (excluding CRC)
 * @return: 16-bit CRC value
 */
static unsigned short Modbus_CRC16(unsigned char * buf, unsigned char length)
{
    unsigned short crc = 0xFFFF;
    unsigned char i, j;
    
    for(i = 0; i < length; i++)
    {
        crc ^= buf[i];
        for(j = 0; j < 8; j++)
        {
            if(crc & 0x0001)
            {
                crc = (crc >> 1) ^ 0xA001;
            }
            else
            {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

/**
 * Initialize MODBUS RTU Slave
 * Initialize register table and receive buffer
 */
void Modbus_Init(unsigned char slave_addr)
{
    unsigned char i;
    
    // Store slave address
    modbus_slave_addr = slave_addr;
    
    // Initialize register table (example values)
    for(i = 0; i < MODBUS_REG_COUNT; i++)
    {
        modbus_holding_regs[i] = 0;
    }
    
    // Initialize read-only registers (3000-3006, mapped to index 0-6)
    modbus_holding_regs[0] = modbus_slave_addr;  // 3000: ADDR: Device RTU address (1-247)
    modbus_holding_regs[1] = 0;  // 3001: Temperature: Will be updated by DHT11
    modbus_holding_regs[2] = 0;  // 3002: Humidity: Will be updated by DHT11
    modbus_holding_regs[3] = 0;  // 3003: PPS: Effective range (start - end)
    modbus_holding_regs[4] = 0;  // 3004: PSDAT: Actual level data
    modbus_holding_regs[5] = 0;  // 3005: BFB: Level percentage (0-100)
    modbus_holding_regs[6] = 0;  // 3006: Error: Error flag (0x00 = no error)
    
    // Initialize read-write registers (4000-4004, mapped to index 7-11)
    modbus_holding_regs[7] = 60;   // 4000: Screen timeout: 60 seconds (1-255, 255=always on)
    modbus_holding_regs[8] = 0x001E;  // 4001: Auto measure interval: 0 minutes 30 seconds (high byte: minutes, low byte: seconds)
    modbus_holding_regs[9] = 0x001E;  // 4002: Auto measure countdown: 30 seconds
    modbus_holding_regs[10] = 5000;   // 4003: Start point: 5000mm (default)
    modbus_holding_regs[11] = 1000;   // 4004: End point: 1000mm (default, must be < start)
    
    modbus_rx_index = 0;
    modbus_frame_ready = 0;
    modbus_last_rx_tick = 0;
}

/**
 * Set holding register value
 * @param reg_addr: Register address (3000-3006 or 4000-4004)
 * @param value: Register value
 */
void Modbus_SetHoldingReg(unsigned short reg_addr, unsigned short value)
{
    // C51 requirement: All variable declarations must be at function start
    unsigned short index = Modbus_AddrToIndex(reg_addr);
    
    if(index < MODBUS_REG_COUNT)
    {
        modbus_holding_regs[index] = value;
    }
}

/**
 * Get holding register value
 * @param reg_addr: Register address (3000-3006 or 4000-4004)
 * @return: Register value (0 if invalid address)
 */
unsigned short Modbus_GetHoldingReg(unsigned short reg_addr)
{
    // C51 requirement: All variable declarations must be at function start
    unsigned short index = Modbus_AddrToIndex(reg_addr);
    unsigned short reg_val;
    
    if(index < MODBUS_REG_COUNT)
    {
        reg_val = modbus_holding_regs[index];
        return reg_val;
    }
    return 0;
}

/**
 * Process received MODBUS RTU frame
 * Should be called periodically from main loop
 * Reads data from UART3 receive buffer and processes complete frames
 */
void Modbus_Process(void)
{
    // C51 requirement: All variable declarations must be at function start
    int recv_char;
    unsigned char slave_addr;
    unsigned char func_code;
    unsigned short start_addr;
    unsigned short reg_count;
    unsigned short crc_calc;
    unsigned short crc_received;
    unsigned char i;
    unsigned char frame_len;
    unsigned char tx_frame[64];
    unsigned char byte_count;
    unsigned char frame_complete = 0;
    unsigned char had_new_data = 0;
    unsigned char rx_count;
    unsigned short index;
    unsigned short reg_val;
    
    // Read characters from UART3 receive buffer (limit to prevent blocking)
    // Process up to 20 characters per call to avoid blocking keys
    rx_count = 0;
    while((recv_char = Uart3_GetCharFromBuffer()) >= 0 && rx_count < 20)
    {
        rx_count++;
        had_new_data = 1;
        if(modbus_rx_index < MODBUS_RX_BUF_SIZE - 1)
        {
            modbus_rx_buffer[modbus_rx_index++] = (unsigned char)recv_char;
            // Record time of the latest received byte
            modbus_last_rx_tick = Systick_GetTick();
        }
        else
        {
            // Buffer overflow, reset
            modbus_rx_index = 0;
            modbus_last_rx_tick = 0;
        }
    }
    
    // If no new data received but we already have some bytes,
    // the frame timeout will be evaluated below using Systick_Elapsed().
    
    // Frame end detection: 3.5 character time timeout
    // If no new data received for MODBUS_FRAME_TIMEOUT_MS, consider frame complete
    // Minimum frame length check: Addr(1) + Func(1) + Data(2) + CRC(2) = 6 bytes
    // For function 0x03/0x06: minimum is 8 bytes
    if(modbus_rx_index >= 6)
    {
        // For function codes 0x03 and 0x06, we need at least 8 bytes
        if(modbus_rx_index >= 4)
        {
            unsigned char detected_func = modbus_rx_buffer[1];
            unsigned char min_len = 6;  // Minimum for any function
            
            if(detected_func == 0x03 || detected_func == 0x06)
            {
                min_len = 8;  // Addr + Func + StartAddr(2) + RegCount/Value(2) + CRC(2)
            }
            else if(detected_func == 0x10)
            {
                // For 0x10, need at least 9 bytes to check byte_count
                if(modbus_rx_index >= 9)
                {
                    unsigned char byte_count = modbus_rx_buffer[6];
                    min_len = 9 + byte_count;  // Full frame length
                }
                else
                {
                    min_len = 9;  // Wait for more data
                }
            }
            
            // Check if frame is complete (has minimum length and timeout occurred)
            if(modbus_rx_index >= min_len &&
               modbus_last_rx_tick != 0 &&
               Systick_Elapsed(modbus_last_rx_tick, MODBUS_FRAME_TIMEOUT_MS))
            {
                frame_complete = 1;
            }
        }
    }
    
    // Process frame if complete
    if(frame_complete)
    {
        frame_len = modbus_rx_index;
        
        // Verify CRC
        crc_calc = Modbus_CRC16(modbus_rx_buffer, frame_len - 2);
        crc_received = modbus_rx_buffer[frame_len - 2] | 
                      (modbus_rx_buffer[frame_len - 1] << 8);
        
        if(crc_calc == crc_received)
        {
            // CRC valid, extract slave address and function code
            slave_addr = modbus_rx_buffer[0];
            func_code = modbus_rx_buffer[1];
            
            // ========================================
            // CRITICAL FIX 1: Check slave address
            // ========================================
            // Only process frames addressed to this slave (or broadcast 0)
            // Broadcast address 0 is only valid for write operations (06/10), not read (03)
            if(slave_addr != modbus_slave_addr && slave_addr != 0)
            {
                // Not for this slave, ignore frame
                modbus_rx_index = 0;
                modbus_last_rx_tick = 0;
                return;
            }
            
            // For broadcast address 0, only allow write operations
            if(slave_addr == 0 && func_code == 0x03)
            {
                // Broadcast read is not allowed, ignore
                modbus_rx_index = 0;
                modbus_last_rx_tick = 0;
                return;
            }
            
            // Process Function Code 0x03: Read Holding Registers
            // Minimum frame: Addr(1) + Func(1) + StartAddr(2) + RegCount(2) + CRC(2) = 8 bytes
            if(func_code == 0x03 && frame_len >= 8)
            {
                start_addr = (modbus_rx_buffer[2] << 8) | modbus_rx_buffer[3];
                reg_count = (modbus_rx_buffer[4] << 8) | modbus_rx_buffer[5];
                
                // Validate request: check address range and count
                if(reg_count > 0 && reg_count <= 125)
                {
                    // Check if all addresses in range are valid
                    unsigned char all_valid = 1;
                    for(i = 0; i < reg_count; i++)
                    {
                        if(!Modbus_IsValidAddr(start_addr + i))
                        {
                            all_valid = 0;
                            break;
                        }
                    }
                    
                    if(all_valid)
                    {
                        // Build response frame
                        tx_frame[0] = slave_addr;
                        tx_frame[1] = 0x03;  // Function code
                        tx_frame[2] = reg_count * 2;  // Byte count
                        
                        // Copy register values (high byte first) using address mapping
                        for(i = 0; i < reg_count; i++)
                        {
                            index = Modbus_AddrToIndex(start_addr + i);
                            reg_val = modbus_holding_regs[index];
                            tx_frame[3 + i * 2] = (reg_val >> 8) & 0xFF;
                            tx_frame[3 + i * 2 + 1] = reg_val & 0xFF;
                        }
                        
                        // Calculate and append CRC
                        byte_count = 3 + reg_count * 2;
                        crc_calc = Modbus_CRC16(tx_frame, byte_count);
                        tx_frame[byte_count] = crc_calc & 0xFF;        // CRC low
                        tx_frame[byte_count + 1] = (crc_calc >> 8) & 0xFF;  // CRC high
                        
                        // Send response via UART3 (non-blocking: queue only, ISR handles transmit)
                        // Try to add all bytes, but don't block if buffer is full
                        for(i = 0; i < byte_count + 2; i++)
                        {
                            if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                            {
                                // Buffer full, trigger send once and try again
                                if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                {
                                    Uart3_SendBatch();
                                }
                                // Try once more, if still full, skip remaining bytes (non-blocking)
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    break;  // Buffer still full, skip remaining bytes
                                }
                            }
                        }

                        // Trigger send once if idle; completion is handled asynchronously
                        if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                        {
                            Uart3_SendBatch();
                        }
                    }
                    else
                    {
                        // Invalid address range - send exception response
                        tx_frame[0] = slave_addr;
                        tx_frame[1] = 0x83;  // Exception function code (0x03 + 0x80)
                        tx_frame[2] = 0x02;  // Exception code: Illegal data address
                        crc_calc = Modbus_CRC16(tx_frame, 3);
                        tx_frame[3] = crc_calc & 0xFF;
                        tx_frame[4] = (crc_calc >> 8) & 0xFF;
                        
                        for(i = 0; i < 5; i++)
                        {
                            if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                            {
                                if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                {
                                    Uart3_SendBatch();
                                }
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    break;  // Buffer still full, skip remaining
                                }
                            }
                        }
                        if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                        {
                            Uart3_SendBatch();
                        }
                    }
                }
            }
            // Process Function Code 0x06: Write Single Register
            else if(func_code == 0x06 && frame_len >= 8)
            {
                start_addr = (modbus_rx_buffer[2] << 8) | modbus_rx_buffer[3];
                reg_count = 1;  // Single register
                
                // Validate address (only allow write to 4000-4004, not 3000-3006)
                if(Modbus_IsValidAddr(start_addr) && start_addr >= 4000)
                {
                    // Special validation for 4003 and 4004: 4003 must be > 4004
                    // According to spec: "均需要保证4003内的值最终大于4004，否则程序会计算出错"
                    unsigned short new_value = (modbus_rx_buffer[4] << 8) | modbus_rx_buffer[5];
                    
                    if(start_addr == 4003)
                    {
                        // Writing to 4003: check if new value > 4004
                        unsigned short current_4004 = modbus_holding_regs[Modbus_AddrToIndex(4004)];
                        if(new_value <= current_4004)
                        {
                            // Invalid: return exception code 0x03 (Illegal Data Value)
                            tx_frame[0] = slave_addr;
                            tx_frame[1] = 0x86;  // Exception function code (0x06 + 0x80)
                            tx_frame[2] = 0x03;  // Exception code: Illegal Data Value
                            crc_calc = Modbus_CRC16(tx_frame, 3);
                            tx_frame[3] = crc_calc & 0xFF;
                            tx_frame[4] = (crc_calc >> 8) & 0xFF;
                            
                            for(i = 0; i < 5; i++)
                            {
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                    {
                                        Uart3_SendBatch();
                                    }
                                    if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                    {
                                        break;
                                    }
                                }
                            }
                            if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                            {
                                Uart3_SendBatch();
                            }
                            // Clear receive buffer after sending exception response
                            modbus_rx_index = 0;
                            modbus_last_rx_tick = 0;
                            return;  // Don't write invalid value
                        }
                    }
                    else if(start_addr == 4004)
                    {
                        // Writing to 4004: check if 4003 > new value
                        unsigned short current_4003 = modbus_holding_regs[Modbus_AddrToIndex(4003)];
                        if(current_4003 <= new_value)
                        {
                            // Invalid: return exception code 0x03 (Illegal Data Value)
                            tx_frame[0] = slave_addr;
                            tx_frame[1] = 0x86;  // Exception function code (0x06 + 0x80)
                            tx_frame[2] = 0x03;  // Exception code: Illegal Data Value
                            crc_calc = Modbus_CRC16(tx_frame, 3);
                            tx_frame[3] = crc_calc & 0xFF;
                            tx_frame[4] = (crc_calc >> 8) & 0xFF;
                            
                            for(i = 0; i < 5; i++)
                            {
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                    {
                                        Uart3_SendBatch();
                                    }
                                    if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                    {
                                        break;
                                    }
                                }
                            }
                            if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                            {
                                Uart3_SendBatch();
                            }
                            // Clear receive buffer after sending exception response
                            modbus_rx_index = 0;
                            modbus_last_rx_tick = 0;
                            return;  // Don't write invalid value
                        }
                    }
                    
                    // Write register value using address mapping
                    index = Modbus_AddrToIndex(start_addr);
                    modbus_holding_regs[index] = new_value;
                    
                    // Echo request as response (non-blocking)
                    for(i = 0; i < frame_len; i++)
                    {
                        if(Uart3_AddToSendBuffer(modbus_rx_buffer[i]) != 0)
                        {
                            if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                            {
                                Uart3_SendBatch();
                            }
                            if(Uart3_AddToSendBuffer(modbus_rx_buffer[i]) != 0)
                            {
                                break;  // Buffer still full, skip remaining
                            }
                        }
                    }
                    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                    {
                        Uart3_SendBatch();
                    }
                }
                else
                {
                    // Illegal data address
                    tx_frame[0] = slave_addr;
                    tx_frame[1] = 0x86;  // Exception function code
                    tx_frame[2] = 0x02;  // Exception code
                    crc_calc = Modbus_CRC16(tx_frame, 3);
                    tx_frame[3] = crc_calc & 0xFF;
                    tx_frame[4] = (crc_calc >> 8) & 0xFF;
                    
                    for(i = 0; i < 5; i++)
                    {
                        // Non-blocking: try to add, if buffer full, trigger send and retry once, then skip
                        if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                        {
                            if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                            {
                                Uart3_SendBatch();
                            }
                            // Retry once
                            if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                            {
                                // Buffer still full, skip remaining bytes to avoid blocking
                                break;
                            }
                        }
                    }
                    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                    {
                        Uart3_SendBatch();
                    }
                    // Clear receive buffer after sending exception response
                    modbus_rx_index = 0;
                    modbus_last_rx_tick = 0;
                }
            }
            // Process Function Code 0x10: Write Multiple Registers
            else if(func_code == 0x10 && frame_len >= 9)
            {
                start_addr = (modbus_rx_buffer[2] << 8) | modbus_rx_buffer[3];
                reg_count = (modbus_rx_buffer[4] << 8) | modbus_rx_buffer[5];
                byte_count = modbus_rx_buffer[6];
                
                // Validate request: check address range, count, and byte count
                if(reg_count > 0 && reg_count <= 123 && 
                   byte_count == reg_count * 2 &&
                   frame_len >= (9 + byte_count))
                {
                    // Check if all addresses in range are valid and writable (4000-4004 only)
                    unsigned char all_valid = 1;
                    for(i = 0; i < reg_count; i++)
                    {
                        if(!Modbus_IsValidAddr(start_addr + i) || (start_addr + i) < 4000)
                        {
                            all_valid = 0;
                            break;
                        }
                    }
                    
                    if(all_valid)
                    {
                        // Validate 4003 and 4004: 4003 must be > 4004
                        // Check if we're writing to 4003 or 4004
                        unsigned short new_4003 = modbus_holding_regs[Modbus_AddrToIndex(4003)];
                        unsigned short new_4004 = modbus_holding_regs[Modbus_AddrToIndex(4004)];
                        
                        // Update values first
                        for(i = 0; i < reg_count; i++)
                        {
                            unsigned short addr = start_addr + i;
                            if(addr == 4003)
                            {
                                new_4003 = (modbus_rx_buffer[7 + i * 2] << 8) | modbus_rx_buffer[8 + i * 2];
                            }
                            else if(addr == 4004)
                            {
                                new_4004 = (modbus_rx_buffer[7 + i * 2] << 8) | modbus_rx_buffer[8 + i * 2];
                            }
                        }
                        
                        // Validate: 4003 must be > 4004
                        if(new_4003 <= new_4004)
                        {
                            // Invalid: return exception code
                            tx_frame[0] = slave_addr;
                            tx_frame[1] = 0x90;  // Exception function code (0x10 + 0x80)
                            tx_frame[2] = 0x03;  // Exception code: Illegal Data Value
                            crc_calc = Modbus_CRC16(tx_frame, 3);
                            tx_frame[3] = crc_calc & 0xFF;
                            tx_frame[4] = (crc_calc >> 8) & 0xFF;
                            
                            for(i = 0; i < 5; i++)
                            {
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                    {
                                        Uart3_SendBatch();
                                    }
                                    if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                    {
                                        break;
                                    }
                                }
                            }
                            if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                            {
                                Uart3_SendBatch();
                            }
                            // Clear receive buffer after sending exception response
                            modbus_rx_index = 0;
                            modbus_last_rx_tick = 0;
                            return;  // Don't write invalid values
                        }
                        
                        // Write register values using address mapping
                        for(i = 0; i < reg_count; i++)
                        {
                            unsigned short index = Modbus_AddrToIndex(start_addr + i);
                            unsigned short write_value = (modbus_rx_buffer[7 + i * 2] << 8) | modbus_rx_buffer[8 + i * 2];
                            modbus_holding_regs[index] = write_value;
                        }
                        
                        // Build response (echo start_addr and reg_count)
                        tx_frame[0] = slave_addr;
                        tx_frame[1] = 0x10;
                        tx_frame[2] = (start_addr >> 8) & 0xFF;
                        tx_frame[3] = start_addr & 0xFF;
                        tx_frame[4] = (reg_count >> 8) & 0xFF;
                        tx_frame[5] = reg_count & 0xFF;
                        crc_calc = Modbus_CRC16(tx_frame, 6);
                        tx_frame[6] = crc_calc & 0xFF;
                        tx_frame[7] = (crc_calc >> 8) & 0xFF;
                        
                        for(i = 0; i < 8; i++)
                        {
                            if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                            {
                                if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                {
                                    Uart3_SendBatch();
                                }
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    break;  // Buffer still full, skip remaining
                                }
                            }
                        }
                        if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                        {
                            Uart3_SendBatch();
                        }
                    }
                    else
                    {
                        // Invalid address range or read-only address - send exception response
                        tx_frame[0] = slave_addr;
                        tx_frame[1] = 0x90;  // Exception function code
                        tx_frame[2] = 0x02;  // Exception code
                        crc_calc = Modbus_CRC16(tx_frame, 3);
                        tx_frame[3] = crc_calc & 0xFF;
                        tx_frame[4] = (crc_calc >> 8) & 0xFF;
                        
                        for(i = 0; i < 5; i++)
                        {
                            if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                            {
                                if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                                {
                                    Uart3_SendBatch();
                                }
                                if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                                {
                                    break;  // Buffer still full, skip remaining
                                }
                            }
                        }
                        if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                        {
                            Uart3_SendBatch();
                        }
                    }
                }
                else
                {
                    // Illegal data address or value
                    tx_frame[0] = slave_addr;
                    tx_frame[1] = 0x90;  // Exception function code
                    tx_frame[2] = 0x02;  // Exception code
                    crc_calc = Modbus_CRC16(tx_frame, 3);
                    tx_frame[3] = crc_calc & 0xFF;
                    tx_frame[4] = (crc_calc >> 8) & 0xFF;
                    
                    for(i = 0; i < 5; i++)
                    {
                        // Non-blocking: try to add, if buffer full, trigger send and retry once, then skip
                        if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                        {
                            if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                            {
                                Uart3_SendBatch();
                            }
                            // Retry once
                            if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                            {
                                // Buffer still full, skip remaining bytes to avoid blocking
                                break;
                            }
                        }
                    }
                    if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                    {
                        Uart3_SendBatch();
                    }
                }
            }
            else
            {
                // Invalid function code - send exception response
                tx_frame[0] = slave_addr;
                tx_frame[1] = func_code | 0x80;  // Exception function code
                tx_frame[2] = 0x01;  // Exception code: Illegal Function
                crc_calc = Modbus_CRC16(tx_frame, 3);
                tx_frame[3] = crc_calc & 0xFF;
                tx_frame[4] = (crc_calc >> 8) & 0xFF;
                
                for(i = 0; i < 5; i++)
                {
                    if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                    {
                        if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                        {
                            Uart3_SendBatch();
                        }
                        if(Uart3_AddToSendBuffer(tx_frame[i]) != 0)
                        {
                            break;
                        }
                    }
                }
                if(Uart3_IsSending() == 0 && Uart3_HasDataToSend())
                {
                    Uart3_SendBatch();
                }
            }
            
            // Clear receive buffer after processing frame
            modbus_rx_index = 0;
            modbus_last_rx_tick = 0;
        }
        else
        {
            // CRC error, clear buffer
            modbus_rx_index = 0;
            modbus_last_rx_tick = 0;
        }
    }
}
