/**
 * GP8202AS DAC Driver - Header
 * 12-bit I2C DAC for 4-20mA / 0-20mA current output.
 * Hardware: A0=A1=A2=GND -> 7-bit addr 0x2C, write 0x58.
 */

#ifndef __GP8202_H
#define __GP8202_H

/* 7-bit addr 0x2C, 8-bit write address (shifted) */
#define GP8202_I2C_ADDR      0x58
/* Data register for channel 0 */
#define GP8202_REG_CH0       0x02
/* 12-bit DAC max value */
#define GP8202_DAC_MAX       4095

void GP8202_Init(void);
unsigned char GP8202_SetDAC(unsigned short dac_value);
unsigned char GP8202_SetPercent(unsigned char percent);

#endif
