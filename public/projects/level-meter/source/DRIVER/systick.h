#ifndef __SYSTICK_H__
#define __SYSTICK_H__

#include <STC8H.H>

// System tick (millisecond counter)
// Uses Timer2 interrupt to generate 1ms ticks
// Timer2 is 16-bit auto-reload timer

// Initialize system tick (1ms interrupt)
void Systick_Init(void);

// Get current tick count (milliseconds since init)
// Returns: milliseconds (32-bit, wraps around after ~49 days)
unsigned long Systick_GetTick(void);

// Check if time has elapsed
// Returns: 1 if elapsed, 0 if not
unsigned char Systick_Elapsed(unsigned long start_tick, unsigned long timeout_ms);

#endif // __SYSTICK_H__

