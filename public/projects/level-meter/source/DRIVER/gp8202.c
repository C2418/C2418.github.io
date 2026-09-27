/**
 * GP8202AS DAC Driver Implementation (ACK-checked software I2C)
 *
 * Datasheet: GP8202AS 12bit DAC I2C to 4-20mA/0-20mA
 * - 3bit hardware address A2 A1 A0; A0=A1=A2=GND -> 7-bit addr 0x2C, 8-bit write 0x58
 * - Register 0x02: write DATA Low then DATA High
 * - DATA Low = low byte (high nibble = D3-D0 of 12-bit; low 4 bits ignored by chip)
 * - DATA High = high byte (D11-D4)
 * - I2C: Start -> DevAddr(W) 0x58 -> 0x02 -> DATA_Low -> DATA_High -> Stop
 *
 * Pins: P1.4 = SDA, P1.5 = SCL (open-drain software I2C)
 */

#include "gp8202.h"
#include "STC8G_H_Soft_I2C.h"
#include "STC8G_H_Switch.h"

/**
 * Initialize GP8202AS DAC
 * Open-drain software I2C on P1.4 (SDA) / P1.5 (SCL); external pull-up required.
 * Each byte is ACK-checked and a missing DAC exits without waiting on a hardware flag.
 */
void GP8202_Init(void)
{
    P_SW2 |= 0x80;
    P1M1 |= 0x30;
    P1M0 |= 0x30;

    P1 |= 0x30;

    /* The bit-banged driver has bounded byte transfers and exits on NACK. */
}

/**
 * Set GP8202AS DAC raw value (12-bit)
 * Data format per datasheet: DATA_Low high nibble = D3-D0, DATA_High = D11-D4.
 * @param dac_value: 12-bit DAC value (0x000 ~ 0xFFF)
 */
unsigned char GP8202_SetDAC(unsigned short dac_value)
{
    u8 i2c_data[2];

    if (dac_value > GP8202_DAC_MAX)
        dac_value = GP8202_DAC_MAX;

    i2c_data[0] = (u8)((dac_value & 0x0F) << 4);
    i2c_data[1] = (u8)((dac_value >> 4) & 0xFF);

    return SI2C_WriteNbyte(GP8202_I2C_ADDR, GP8202_REG_CH0, i2c_data, 2);
}

/**
 * Set GP8202AS output by level percentage (0-100% -> 4-20mA)
 */
unsigned char GP8202_SetPercent(unsigned char percent)
{
    unsigned short dac_value;

    if (percent > 100)
        percent = 100;
    dac_value = (unsigned short)(((unsigned long)percent * 4095UL) / 100UL);
    return GP8202_SetDAC(dac_value);
}
