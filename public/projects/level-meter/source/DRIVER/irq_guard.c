#include <STC8H.H>
#include "irq_guard.h"

unsigned char IRQ_Save(void)
{
    unsigned char prev = EA;
    EA = 0;
    return prev;
}

void IRQ_Restore(unsigned char prev)
{
    EA = prev;
}

