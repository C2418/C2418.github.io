#include "laser.h"
#include "uart.h"
#include "systick.h"
#include "cmd_queue.h"
#include "delay.h"

/**
 * Laser Module Hardware Enable Pin Initialization
 * Pin: P0.5 (LD_ON)
 * 
 * Configuration:
 * - P0M1[5] = 0, P0M0[5] = 1 → Push-pull output mode
 * - P05 = 1 → High level (enable laser hardware)
 * 
 * IMPORTANT: This function MUST be called before Uart4_Init()
 * to ensure hardware enable pin is configured first.
 */
void Laser_Init()
{
    // Configure P0.5 as push-pull output mode
    // P0M1[5]=0, P0M0[5]=1 → Push-pull output (strong drive capability)
    P0M0 |= 0x20;    // Set P0M0 bit5 = 1 (0x20 = 0010 0000)
    P0M1 &= ~0x20;   // Clear P0M1 bit5 = 0
    
    // Ensure P0.5 is high (enable laser hardware)
    P0 |= 0x20;      // Set P05 = 1
}


void Laser_On(void)
{
    // Use async command queue on UART4 (non-blocking, interrupt-based)
    // UART4 uses interrupt mode for both TX and RX
    CmdQueue_Push("<mAo>", 0, 1000);
}


void Laser_Off(void)
{
    // Use async command queue on UART4 (non-blocking, interrupt-based)
    // UART4 uses interrupt mode for both TX and RX
    CmdQueue_Push("<mAp>", 1, 1000);
}