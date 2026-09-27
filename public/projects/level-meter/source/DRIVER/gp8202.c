/**
 * GP8202AS DAC Driver Implementation (Hardware I2C)
 *
 * Datasheet: GP8202AS 12bit DAC I2C to 4-20mA/0-20mA
 * - 3bit hardware address A2 A1 A0; A0=A1=A2=GND -> 7-bit addr 0x2C, 8-bit write 0x58
 * - Register 0x02: write DATA Low then DATA High
 * - DATA Low = low byte (high nibble = D3-D0 of 12-bit; low 4 bits ignored by chip)
 * - DATA High = high byte (D11-D4)
 * - I2C: Start -> DevAddr(W) 0x58 -> 0x02 -> DATA_Low -> DATA_High -> Stop
 *
 * Pins: P1.4 = SDA, P1.5 = SCL (hardware I2C)
 */

#include "gp8202.h"
#include "STC8G_H_I2C.h"
#include "STC8G_H_Switch.h"

static I2C_InitTypeDef I2C_InitStruct;

/**
 * Initialize GP8202AS DAC
 * Hardware I2C on P1.4 (SDA) / P1.5 (SCL). EAXSFR() must be called in main() first.
 * Order: I2C_Init() then I2C_SW(I2C_P14_P15). P1.4/P1.5 open-drain, external pull-up required.
 */
void GP8202_Init(void)
{
    P_SW2 |= 0x80;
    P1M1 |= 0x30;
    P1M0 |= 0x30;

    I2C_InitStruct.I2C_Speed    = 28;   /* ~200kHz at 24MHz */
    I2C_InitStruct.I2C_Enable   = ENABLE;
    I2C_InitStruct.I2C_Mode     = I2C_Mode_Master;
    I2C_InitStruct.I2C_MS_WDTA  = DISABLE;
    I2C_InitStruct.I2C_SL_ADR   = 0;
    I2C_InitStruct.I2C_SL_MA    = DISABLE;
    I2C_Init(&I2C_InitStruct);
    I2C_SW(I2C_P14_P15);

    /* Do not write DAC here: if bus/GP8202 is missing or NACK, I2C Wait() would hang and cause WDT reset.
     * First write happens in App_ProcessCurrentOutput (MODBUS 3005) after ~500ms. */
}

/**
 * Set GP8202AS DAC raw value (12-bit)
 * Data format per datasheet: DATA_Low high nibble = D3-D0, DATA_High = D11-D4.
 * @param dac_value: 12-bit DAC value (0x000 ~ 0xFFF)
 */
void GP8202_SetDAC(unsigned short dac_value)
{
    u8 i2c_data[2];

    if (dac_value > GP8202_DAC_MAX)
        dac_value = GP8202_DAC_MAX;

    i2c_data[0] = (u8)((dac_value & 0x0F) << 4);
    i2c_data[1] = (u8)((dac_value >> 4) & 0xFF);

    P_SW2 |= 0x80;
    I2C_WriteNbyte(GP8202_I2C_ADDR, GP8202_REG_CH0, i2c_data, 2);
}

/**
 * Set GP8202AS output by level percentage (0-100% -> 4-20mA)
 */
void GP8202_SetPercent(unsigned char percent)
{
    unsigned short dac_value;

    if (percent > 100)
        percent = 100;
    dac_value = (unsigned short)(((unsigned long)percent * 4095UL) / 100UL);
    GP8202_SetDAC(dac_value);
}
