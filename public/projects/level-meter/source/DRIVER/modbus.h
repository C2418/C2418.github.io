#ifndef __MODBUS_H__
#define __MODBUS_H__

#include <STC8H.H>

/**
 * MODBUS RTU Slave Implementation for STC8H MCU
 * 
 * MCU acts as Modbus-RTU slave, responding to requests from host.
 * NE2 module converts Modbus-TCP (from host) <-> Modbus-RTU (to MCU).
 * 
 * MODBUS RTU Frame Format:
 * [Slave Address] [Function Code] [Data...] [CRC16 Low] [CRC16 High]
 * 
 * Supported Function Codes:
 * - 0x03: Read Holding Registers
 * - 0x06: Write Single Register
 * - 0x10: Write Multiple Registers
 * 
 * Frame Detection:
 * - Uses 3.5 character time timeout for frame end detection
 * - At 9600bps: 3.5T ≈ 3.65ms
 * - At 19200bps: 3.5T ≈ 1.82ms
 */

/**
 * Initialize MODBUS RTU Slave
 * @param slave_addr: MODBUS slave address (1-247, 0 is broadcast)
 */
void Modbus_Init(unsigned char slave_addr);

/**
 * Set holding register value
 * @param reg_addr: Register address (0 to MODBUS_REG_COUNT-1)
 * @param value: Register value
 */
void Modbus_SetHoldingReg(unsigned short reg_addr, unsigned short value);

/**
 * Get holding register value
 * @param reg_addr: Register address (0 to MODBUS_REG_COUNT-1)
 * @return: Register value
 */
unsigned short Modbus_GetHoldingReg(unsigned short reg_addr);

/**
 * Process received MODBUS RTU frame
 * Should be called periodically from main loop
 * Reads data from UART3 receive buffer and processes complete frames
 */
void Modbus_Process(void);

#endif  // __MODBUS_H__
