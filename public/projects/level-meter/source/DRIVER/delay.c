#include <intrins.h>
#include "delay.h"
#include <STC8H.H>
#define FOSC 24000000L
#define DELAY_US_COUNT (FOSC / 1000000L)  // 1us



//The following functiongs can be used.

void Delay_us(unsigned int us)
{
    while(us--)
    {
        // 共 24 个 NOP
    }
}


void Delay_ms(unsigned int ms)
{
    while(ms--)
    {
        Delay_us(1000);   // 在 Delay_us 已修正的前提下
    }
}

