#include "NE2.h"
#include <STC8H.H>

/**
 * NE2 Network Module GPIO Initialization
 * Configure GPIO mode for NE2 related pins
 * 
 * 注意：实际硬件测试发现，STATE 引脚逻辑为：
 * - STATE=1（高电平）：网线已连接
 * - STATE=0（低电平）：网线未连接
 * 
 * 外部硬件已经有上拉电阻，不需要启用内部上拉
 */
void NE2_Init(void)
{
    // P4.1: NE2_RST - Reset pin, MCU does not control, pulled up internally by NE2
    // Configure as high-impedance input mode (external pull-up)
    P4M0 &= ~0x02;  // Clear P4.1 P4M0 bit (bit1=0)
    P4M1 |= 0x02;   // Set P4.1 P4M1 bit (bit1=1) - High-impedance input
    
    // P3.5: NE2_STATE0 - 10M network status indicator
    // Configure as high-impedance input mode (external pull-up exists, no internal pull-up needed)
    P3M0 &= ~0x20;  // Clear P3.5 P3M0 bit (bit5=0)
    P3M1 |= 0x20;   // Set P3.5 P3M1 bit (bit5=1) - High-impedance input
    // 不启用内部上拉（外部已有上拉电阻）
    
    // P3.6: NE2_STATE1 - 100M network status indicator
    // Configure as high-impedance input mode (external pull-up exists, no internal pull-up needed)
    P3M0 &= ~0x40;  // Clear P3.6 P3M0 bit (bit6=0)
    P3M1 |= 0x40;   // Set P3.6 P3M1 bit (bit6=1) - High-impedance input
    // 不启用内部上拉（外部已有上拉电阻）
}

/**
 * Read NE2_STATE0/1 status (10M/100M network status)
 * 注意：实际硬件测试发现，STATE 引脚逻辑为：1=已连接，0=未连接
 * Return: 1=connected, 0=not connected
 */
unsigned char NE2_GetState0(void)
{
    return (P35) ? 1 : 0;  // P3.5 pin level (10M status: 1=connected)
}

unsigned char NE2_GetState1(void)
{
    return (P36) ? 1 : 0;  // P3.6 pin level (100M status: 1=connected)
}

/**
 * Check if network is connected (either 10M or 100M)
 * Return: 1=connected, 0=not connected
 * 
 * NE2模块网络连接检测逻辑（基于实际硬件测试）：
 * - STATE0=1, STATE1=1：已连接
 * - STATE0=0, STATE1=0：未连接
 * 
 * 去抖机制：连续稳定5次才改变显示状态，避免通信时瞬态变化导致图标闪烁
 */
unsigned char NE2_IsConnected(void)
{
    static unsigned char stable_state = 0;        // 稳定的显示状态
    static unsigned char last_raw_state = 0;      // 上次原始状态
    static unsigned char stable_count = 0;        // 稳定计数器
    
    unsigned char state0 = NE2_GetState0();
    unsigned char state1 = NE2_GetState1();
    unsigned char raw_state;
    
    // 判断当前原始状态
    if(state0 && state1)
    {
        raw_state = 1;  // 已连接
    }
    else if(!state0 && !state1)
    {
        raw_state = 0;  // 未连接
    }
    else
    {
        raw_state = stable_state;  // 不确定，保持上次稳定状态
    }
    
    // 去抖逻辑：连续稳定5次才改变显示状态
    if(raw_state == last_raw_state)
    {
        // 状态保持不变，增加计数
        if(stable_count < 5)
        {
            stable_count++;
        }
        
        // 连续稳定5次，更新显示状态
        if(stable_count >= 5)
        {
            stable_state = raw_state;
        }
    }
    else
    {
        // 状态变化，重置计数
        last_raw_state = raw_state;
        stable_count = 0;
    }
    
    return stable_state;
}

/**
 * Get raw state values for debugging
 * 用于调试：获取实际的引脚电平状态
 * @param state0: 输出参数，STATE0引脚电平（0或1）
 * @param state1: 输出参数，STATE1引脚电平（0或1）
 */
void NE2_GetRawState(unsigned char* state0, unsigned char* state1)
{
    if(state0 != 0)
    {
        *state0 = NE2_GetState0();
    }
    if(state1 != 0)
    {
        *state1 = NE2_GetState1();
    }
}

