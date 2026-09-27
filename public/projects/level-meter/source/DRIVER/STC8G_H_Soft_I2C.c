/*---------------------------------------------------------------------*/
/* --- STC MCU Limited ------------------------------------------------*/
/* --- STC 1T Series MCU Demo Programme -------------------------------*/
/* --- Mobile: (86)13922805190 ----------------------------------------*/
/* --- Fax: 86-0513-55012956,55012947,55012969 ------------------------*/
/* --- Tel: 86-0513-55012928,55012929,55012966 ------------------------*/
/* --- Web: www.STCAI.com ---------------------------------------------*/
/* --- BBS: www.STCAIMCU.com  -----------------------------------------*/
/* --- QQ:  800003751 -------------------------------------------------*/
/* ���Ҫ�ڳ�����ʹ�ô˴���,���ڳ�����ע��ʹ����STC�����ϼ�����            */
/*---------------------------------------------------------------------*/

#include	"STC8G_H_Soft_I2C.h"

sbit    SDA = P1^4; //����SDA (GP8202)
sbit    SCL = P1^5; //����SCL (GP8202)

//========================================================================
// ����: void I2C_Delay(void)
// ����: I2C��ʱ����.
// ����: none.
// ����: none.
// �汾: V1.0, 2020-09-15
//========================================================================
void I2C_Delay(void) //for normal MCS51, delay (2 * dly + 4) T, for STC12Cxxxx delay (4 * dly + 10) T
{
	u8  dly;
	dly = MAIN_Fosc / 1200000UL;   /* ~100kHz at 24MHz */        //��2us����
	while(--dly);
}

//========================================================================
// I2C���ߺ���
//========================================================================
void I2C_Start(void)         //start the I2C, SDA High-to-low when SCL is high
{
	SDA = 1;
	I2C_Delay();
	SCL = 1;
	I2C_Delay();
	SDA = 0;
	I2C_Delay();
	SCL = 0;
	I2C_Delay();
}       


void I2C_Stop(void)           //STOP the I2C, SDA Low-to-high when SCL is high
{
	SDA = 0;
	I2C_Delay();
	SCL = 1;
	I2C_Delay();
	SDA = 1;
	I2C_Delay();
}

void S_ACK(void)              //Send ACK (LOW)
{
	SDA = 0;
	I2C_Delay();
	SCL = 1;
	I2C_Delay();
	SCL = 0;
	I2C_Delay();
}

void S_NoACK(void)           //Send No ACK (High)
{
	SDA = 1;
	I2C_Delay();
	SCL = 1;
	I2C_Delay();
	SCL = 0;
	I2C_Delay();
}
        
void I2C_Check_ACK(void)     //Check ACK, If F0=0, then right, if F0=1, then error
{
	SDA = 1;
	I2C_Delay();
	SCL = 1;
	I2C_Delay();
	I2C_Delay();   /* sample mid pulse so slave has time to pull SDA low */
	F0  = SDA;
	SCL = 0;
	I2C_Delay();
}

//========================================================================
// ����: void I2C_WriteAbyte(u8 dat)
// ����: I2C����һ���ֽ����ݺ���.
// ����: ���͵�����.
// ����: none.
// �汾: V1.0, 2020-09-15
//========================================================================
void I2C_WriteAbyte(u8 dat)     //write a byte to I2C
{
	u8 i;
	i = 8;
	do
	{
		if(dat & 0x80)  SDA = 1;
		else            SDA = 0;
		dat <<= 1;
		I2C_Delay();
		SCL = 1;
		I2C_Delay();
		SCL = 0;
		I2C_Delay();
	}
	while(--i);
}

//========================================================================
// ����: u8 I2C_ReadAbyte(void)
// ����: I2C��ȡһ���ֽ����ݺ���.
// ����: none.
// ����: ��ȡ����.
// �汾: V1.0, 2020-09-15
//========================================================================
u8 I2C_ReadAbyte(void)          //read A byte from I2C
{
	u8 i,dat;
	i = 8;
	SDA = 1;
	do
	{
		SCL = 1;
		I2C_Delay();
		dat <<= 1;
		if(SDA)     dat++;
		SCL  = 0;
		I2C_Delay();
	}
	while(--i);
	return(dat);
}

//========================================================================
// ����: void SI2C_WriteNbyte(u8 dev_addr, u8 mem_addr, u8 *p, u8 number)
// ����: I2Cд�����ݺ���.
// ����: dev_addr: �豸��ַ, mem_addr: �洢��ַ, *pд�����ݴ洢λ��, numberд�����ݸ���.
// ����: none.
// �汾: V1.0, 2020-09-15
//========================================================================
void SI2C_WriteNbyte(u8 dev_addr, u8 mem_addr, u8 *p, u8 number)  /*  DeviceAddress,WordAddress,First Data Address,Byte lenth  */
{
	I2C_Start();
	I2C_WriteAbyte(dev_addr);
	I2C_Check_ACK();
	if(!F0)                                           //F0=0,right, F0=1,error
	{
		I2C_WriteAbyte(mem_addr);
		I2C_Check_ACK();
		if(!F0)
		{
			do
			{
				I2C_WriteAbyte(*p); p++;
				I2C_Check_ACK();
				if(F0)  break;
			}
			while(--number);
		}
	}
	I2C_Stop();
}

//========================================================================
// ����: void SI2C_ReadNbyte(u8 dev_addr, u8 mem_addr, u8 *p, u8 number)
// ����: I2C��ȡ���ݺ���.
// ����: dev_addr: �豸��ַ, mem_addr: �洢��ַ, *p��ȡ���ݴ洢λ��, number��ȡ���ݸ���.
// ����: none.
// �汾: V1.0, 2020-09-15
// SI2C_WriteNbyte_Force: send full frame without breaking on NACK (for test)
void SI2C_WriteNbyte_Force(u8 dev_addr, u8 mem_addr, u8 *p, u8 number)
{
	I2C_Start();
	I2C_WriteAbyte(dev_addr);
	I2C_Check_ACK();
	I2C_WriteAbyte(mem_addr);
	I2C_Check_ACK();
	do
	{
		I2C_WriteAbyte(*p); p++;
		I2C_Check_ACK();
	}
	while(--number);
	I2C_Stop();
}

//========================================================================
void SI2C_ReadNbyte(u8 dev_addr, u8 mem_addr, u8 *p, u8 number)  /*  DeviceAddress,WordAddress,First Data Address,Byte lenth  */
{
	I2C_Start();
	I2C_WriteAbyte(dev_addr);
	I2C_Check_ACK();
	if(!F0)
	{
		I2C_WriteAbyte(mem_addr);
		I2C_Check_ACK();
		if(!F0)
		{
			I2C_Start();
			I2C_WriteAbyte(dev_addr|1);
			I2C_Check_ACK();
			if(!F0)
			{
				do
				{
					*p = I2C_ReadAbyte();   p++;
					if(number != 1)     S_ACK();    //send ACK
				}
				while(--number);
				S_NoACK();          //send no ACK
			}
		}
	}
	I2C_Stop();
}
