#include "NE2.h"
#include <STC8H.H>

/**
 * NE2 Network Module GPIO Initialization
 * Configure GPIO mode for NE2 related pins
 */
void NE2_Init(void)
{
    // P4.1: NE2_RST - Reset pin, MCU does not control, pulled up internally by NE2
    // Configure as high-impedance input mode
    P4M0 &= ~0x02;  // Clear P4.1 P4M0 bit (bit1=0)
    P4M1 &= ~0x02;  // Clear P4.1 P4M1 bit (bit1=0)
    
    // P3.5: NE2_STATE0 - 10M network status indicator (1=not connected, 0=connected)
    // Configure as high-impedance input mode
    P3M0 &= ~0x20;  // Clear P3.5 P3M0 bit (bit5=0)
    P3M1 &= ~0x20;  // Clear P3.5 P3M1 bit (bit5=0)
    
    // P3.6: NE2_STATE1 - 100M network status indicator (1=not connected, 0=connected)
    // Configure as high-impedance input mode
    P3M0 &= ~0x40;  // Clear P3.6 P3M0 bit (bit6=0)
    P3M1 &= ~0x40;  // Clear P3.6 P3M1 bit (bit6=0)
}

/**
 * Read NE2_STATE0/1 status (10M/100M network status)
 * These helpers are not used in the current firmware and are wrapped in
 * #if 0 to avoid uncalled segment warnings from the linker.
 */
#if 0
unsigned char NE2_GetState0(void)
{
    return (P35) ? 1 : 0;  // P3.5 pin level
}

unsigned char NE2_GetState1(void)
{
    return (P36) ? 1 : 0;  // P3.6 pin level
}
#endif

