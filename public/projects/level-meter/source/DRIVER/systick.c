#include "systick.h"
#include "irq_guard.h"
#include <STC8H.H>

// System tick counter (incremented by Timer2 interrupt every 1ms)
static volatile unsigned long systick_counter = 0;

// Timer2 interrupt handler (1ms tick)
// Timer2 interrupt vector: 12 (0x0063H)
// Note: If STC8G_H_Timer_Isr.c is used, merge this into Timer2_ISR_Handler()
// Note: In STC8H, Timer2 interrupt flag is automatically cleared in auto-reload mode
void Systick_Timer2_ISR(void) interrupt TMR2_VECTOR
{
    systick_counter++;
    // Timer2 interrupt flag is automatically cleared by hardware in auto-reload mode
    // No need to manually clear TF2 (which doesn't exist in STC8H)
}

/**
 * Initialize system tick using Timer2
 * Timer2: 16-bit auto-reload, 1T mode
 * At 24MHz: 1ms = 24000 counts
 * Reload value = 65536 - 24000 = 41536 (0xA240)
 */
void Systick_Init(void)
{
    // Stop Timer2
    AUXR &= ~0x10;  // TR2 = 0
    
    // Timer2: 16-bit auto-reload, 1T mode
    AUXR &= ~0x08;  // Timer2 as timer
    AUXR |= 0x04;   // Timer2 1T mode
    
    // Set reload value for 1ms @ 24MHz
    // 1ms = 24000 counts, reload = 65536 - 24000 = 41536
    T2H = 0xA2;     // High byte of 41536
    T2L = 0x40;     // Low byte of 41536
    
    // Timer2 interrupt flag is automatically cleared by hardware in auto-reload mode
    // No need to manually clear TF2 (which doesn't exist in STC8H)
    
    // Initialize counter
    systick_counter = 0;
    
    // Enable Timer2 interrupt
    IE2 |= 0x04;    // ET2 = 1 (enable Timer2 interrupt)
    
    // Enable global interrupt
    EA = 1;
    
    // Start Timer2
    AUXR |= 0x10;   // TR2 = 1
}

/**
 * Get current tick count (milliseconds since init)
 */
unsigned long Systick_GetTick(void)
{
    unsigned long tick;
    unsigned char _ea = IRQ_Save();  // Disable interrupt
    tick = systick_counter;
    IRQ_Restore(_ea);  // Enable interrupt
    return tick;
}

/**
 * Check if time has elapsed
 * @param start_tick: Start tick value
 * @param timeout_ms: Timeout in milliseconds
 * @return: 1 if elapsed, 0 if not
 */
unsigned char Systick_Elapsed(unsigned long start_tick, unsigned long timeout_ms)
{
    unsigned long current = Systick_GetTick();
    unsigned long elapsed;
    
    // Handle wrap-around (after ~49 days)
    if(current >= start_tick)
    {
        elapsed = current - start_tick;
    }
    else
    {
        // Wrapped around
        elapsed = (0xFFFFFFFFUL - start_tick) + current + 1;
    }
    
    return (elapsed >= timeout_ms) ? 1 : 0;
}

