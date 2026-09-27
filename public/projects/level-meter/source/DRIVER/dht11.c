// dht11.c
#include "dht11.h"
#include "delay.h"
#include "irq_guard.h"
#include <intrins.h>

// DHT11数据存储
unsigned char dht11_data[4];  // [0]:湿度整数, [1]:湿度小数, [2]:温度整数, [3]:温度小数

/**
 * 初始化DHT11（配置P1.0为推挽输出）
 */
void DHT11_Init(void)
{
    // P1.0配置为推挽输出
    P1M0 |= 0x01;   // P1M0 bit0 = 1
    P1M1 &= ~0x01;  // P1M1 bit0 = 0
    DHT11_DATA = 1; // 初始高电平
}

/**
 * 设置P1.0为输出模式（推挽输出）
 */
void DHT11_IO_Out(void)
{
    P1M0 |= 0x01;   // P1M0 bit0 = 1 (推挽输出)
    P1M1 &= ~0x01;  // P1M1 bit0 = 0
}

/**
 * 设置P1.0为输入模式（高阻输入，启用内部上拉）
 * 注意：DHT11直接连接到MCU的IO引脚
 * 虽然外部有4.7k上拉电阻，但启用内部上拉可以：
 * 1. 确保IO引脚在切换到输入模式后立即保持高电平
 * 2. 避免IO引脚浮空导致的不确定状态
 * 3. 内部上拉和外部上拉并联，总电阻更小，驱动能力更强
 * 
 * 关键：在STC8H上，如果IO引脚之前是推挽输出，切换到输入模式前
 * 必须先写1到锁存器，否则DATA线可能被"弱拉低"，DHT11永远不会响应
 */
void DHT11_IO_In(void)
{
    // 关键步骤1：先确保锁存器写1（即使当前是输出模式）
    // 这样可以避免某些STC8H批次芯片在切换模式时DATA线被弱拉低
    DHT11_DATA = 1;     // 先写1到锁存器，确保DATA线为高
    
    // 关键步骤2：再切换为高阻输入模式
    P1M0 &= ~0x01;  // P1M0 bit0 = 0
    P1M1 |= 0x01;   // P1M1 bit0 = 1 (高阻输入)
    
    // 关键步骤3：启用内部上拉电阻
    // 内部上拉和外部4.7k上拉并联，总电阻更小，确保DATA线稳定在高电平
    P1PU |= 0x01;   // 启用P1.0内部上拉电阻
    
    // 注意：切换后，DATA线应该被内部上拉和外部上拉共同拉高
    // 如果DHT11响应，DHT11会拉低DATA，MCU应该能读到低电平
    // 内部上拉电阻通常为10k-50k，与外部4.7k并联后总电阻约3k-4k，驱动能力足够
}

/**
 * DHT11启动信号
 * 标准DHT11启动序列：
 *   1. DATA保持高电平（空闲状态，通过上拉电阻）
 *   2. MCU拉低DATA至少18ms（启动信号）
 *   3. MCU拉高DATA 20-40us（释放，等待DHT11响应）
 *   4. MCU切换到输入模式，等待DHT11响应
 * 
 * 关键点：
 *   - 释放时必须先写1，再切换输入模式
 *   - 切换输入模式前，确保锁存器写1（在DHT11_IO_In()中完成）
 */
void DHT11_Start(void)
{
    DHT11_IO_Out();     // 设置为输出模式（推挽输出）
    
    // 步骤1：确保DATA为高电平（空闲状态）
    DHT11_DATA = 1;     // MCU输出高 -> DHT11 DATA高（空闲状态，通过上拉电阻）
    Delay_ms(2);        // 等待稳定（确保DATA线稳定在高电平）
    
    // 步骤2：MCU拉低DATA（启动信号）
    DHT11_DATA = 0;     // MCU输出低 -> DHT11 DATA低（启动信号）
    // DHT11标准：MCU拉低至少18ms，最大不得超过30ms
    // 使用18ms（最小值），确保符合标准，同时给MCU释放后留出足够时间
    Delay_ms(15);       // 至少18ms，最大不得超过30ms（使用18ms最小值）
    
    // 步骤3：MCU拉高DATA（释放，等待DHT11响应）
    DHT11_DATA = 1;     // MCU输出高 -> DHT11 DATA高（释放，等待响应，通过上拉电阻）
    Delay_us(40);       // 20-40us（使用40us上限，给DHT11更多响应时间）
    
    // 步骤4：切换到输入模式（关键：DHT11_IO_In()会先写1再切换）
    DHT11_IO_In();      // 切换为输入模式，等待DHT11响应
    // 注意：DHT11_IO_In()内部会先写1到锁存器，再切换为高阻输入
    // 这样可以避免某些STC8H批次芯片在切换模式时DATA线被弱拉低
    
    // 步骤5：等待IO模式切换稳定
    Delay_us(10);       // 等待IO模式切换稳定（给硬件一点时间）
    
    // 此时状态：
    // - IO引脚为高阻输入模式
    // - DATA线应该被外部上拉电阻拉高（如果上拉电阻存在且正常）
    // - 如果DHT11没有响应，MCU应该读到高电平（通过外部上拉电阻）
    // - 如果DHT11响应了，DHT11会拉低DATA，MCU应该能读到低电平
}

/**
 * 读取DHT11返回的一个字节
 */
unsigned char DHT11_Read_Byte(void)
{
    unsigned char i;
    unsigned char dat = 0;
    unsigned char timeout;
    
    for(i = 0; i < 8; i++)
    {
        // 等待低电平结束（数据位开始，约50us）
        timeout = 0;
        while(!DHT11_DATA)
        {
            Delay_us(1);
            timeout++;
            if(timeout > 100)  // 超时约100us
            {
                return 0xFF;  // 返回错误值
            }
        }
        
        // 延时28us判断数据位（DHT11标准：26-28us后判断）
        // 如果26-28us后仍为高，则为'1'（高电平持续约70us），否则为'0'（高电平持续约26-28us）
        // 使用28us作为判断点，更接近标准值
        Delay_us(28);
        
        dat <<= 1;      // 左移一位
        
        if(DHT11_DATA)  // 如果此时仍为高电平，则为'1'
        {
            dat |= 1;
        }
        
        // 等待高电平结束
        timeout = 0;
        while(DHT11_DATA)
        {
            Delay_us(1);
            timeout++;
            if(timeout > 100)  // 超时约100us
            {
                return 0xFF;  // 返回错误值
            }
        }
    }
    
    return dat;
}

/**
 * 读取DHT11完整数据
 * 返回0: 成功
 * 返回1: 失败 - 等待响应低电平超时
 * 返回2: 失败 - 等待响应高电平超时
 * 返回3: 失败 - 校验和错误
 * 
 * 注意：DHT11 通信需要精确的微秒级时序，必须在禁用中断的情况下进行
 */
unsigned char DHT11_Read_Data(void)
{
    unsigned char R_H, R_L, T_H, T_L, checksum;
    unsigned char timeout;
    unsigned char irq_state;
    
    // 启动DHT11（这部分可以有中断，时序要求不严格）
    DHT11_Start();
    
    // 关键时序部分：禁用中断，确保时序精确
    // DHT11 通信协议要求微秒级精度，任何中断都会导致时序错乱
    irq_state = IRQ_Save();
    
    // 等待DHT11响应：传感器拉低83µs（响应信号）
    // DHT11标准：MCU释放后，DHT11应在20-200us内拉低响应
    // 增加超时时间以适配实际硬件响应时间
    timeout = 0;
    while(DHT11_DATA)  // 等待DHT11拉低（响应信号）
    {
        Delay_us(100);  // 使用100us步进，减少循环次数
        timeout++;
        if(timeout > 200)  // 超时约20ms（给足够余量，适配实际硬件）
        {
            // 保存错误数据
            dht11_data[0] = 0xFF;
            dht11_data[1] = 0xFF;
            dht11_data[2] = 0xFF;
            dht11_data[3] = 0xFF;
            IRQ_Restore(irq_state);  // 恢复中断
            return 1;  // DHT11未响应（启动后没有拉低，可能连接问题或DHT11故障）
        }
    }
    
    // 如果到这里，说明DHT11已经拉低响应了
    // 等待低电平结束（DHT11响应信号：拉低83µs）
    timeout = 0;
    while(!DHT11_DATA)
    {
        Delay_us(5);    // 更细粒度的检测
        timeout++;
        if(timeout > 200)  // 超时约1000us（DHT11响应低电平应该约83us，给足够余量）
        {
            // 保存错误数据
            dht11_data[0] = 0xFF;
            dht11_data[1] = 0xFF;
            dht11_data[2] = 0xFF;
            dht11_data[3] = 0xFF;
            IRQ_Restore(irq_state);  // 恢复中断
            return 1;  // 超时失败 - 等待响应低电平结束超时
        }
    }
    
    // 等待DHT11拉高87µs（响应信号：接高87µs）
    timeout = 0;
    while(DHT11_DATA)
    {
        Delay_us(5);    // 更细粒度的检测
        timeout++;
        if(timeout > 200)  // 超时约1000us（DHT11响应高电平应该约87us，给足够余量）
        {
            // 即使超时，也尝试读取数据 - DHT11可能已经响应
            // 继续读取字节并检查是否获得有效数据
            break;  // 不立即返回，先尝试读取数据
        }
    }
    
    // 读取5个字节：湿度高、湿度低、温度高、温度低、校验和
    R_H = DHT11_Read_Byte();   // 湿度整数部分
    R_L = DHT11_Read_Byte();   // 湿度小数部分
    T_H = DHT11_Read_Byte();   // 温度整数部分
    T_L = DHT11_Read_Byte();   // 温度小数部分
    checksum = DHT11_Read_Byte(); // 校验和
    
    // 关键时序部分结束：恢复中断
    IRQ_Restore(irq_state);
    
    // 检查是否有字节读取超时
    if(R_H == 0xFF || R_L == 0xFF || T_H == 0xFF || T_L == 0xFF || checksum == 0xFF)
    {
        // 保存原始数据（即使超时）
        dht11_data[0] = R_H;
        dht11_data[1] = R_L;
        dht11_data[2] = T_H;
        dht11_data[3] = T_L;
        return 2; // 返回2表示读取字节超时
    }
    
    // 保存原始数据（无论是否成功）
    dht11_data[0] = R_H;
    dht11_data[1] = R_L;
    dht11_data[2] = T_H;
    dht11_data[3] = T_L;
    
    // 校验数据
    if((R_H + R_L + T_H + T_L) == checksum)
    {
        return 0;  // 成功
    }
    
    return 3;  // 校验失败
}
