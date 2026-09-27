// dht11.h
#ifndef __DHT11_H__
#define __DHT11_H__

#include <STC8H.H>

// DHT11数据引脚定义
sbit DHT11_DATA = P1^0;  // P1.0作为DHT11数据引脚

// DHT11数据结构
// rec_data[0]: 湿度整数部分
// rec_data[1]: 湿度小数部分
// rec_data[2]: 温度整数部分
// rec_data[3]: 温度小数部分
extern unsigned char dht11_data[4];

// 函数声明
void DHT11_Init(void);                    // 初始化DHT11
void DHT11_IO_Out(void);                  // 设置P1.0为输出模式
void DHT11_IO_In(void);                   // 设置P1.0为输入模式
void DHT11_Start(void);                   // 启动DHT11
unsigned char DHT11_Read_Byte(void);      // 读取一个字节
unsigned char DHT11_Read_Data(void);      // 读取完整数据，返回0成功，1失败

#endif
