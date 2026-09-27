#ifndef __NE2_H__
#define __NE2_H__

#include <STC8H.H>

/**
 * NE2 Network Module GPIO Initialization
 * Configure pins:
 * - P4.1: NE2_RST (Reset pin, high-impedance input, MCU does not control)
 * - P3.5: NE2_STATE0 (10M network status indicator, high-impedance input)
 * - P3.6: NE2_STATE1 (100M network status indicator, high-impedance input)
 */
void NE2_Init(void);

/**
 * Read NE2 network status
 * Return: 1=not connected, 0=connected
 */
unsigned char NE2_GetState0(void);  // 10M network status
unsigned char NE2_GetState1(void);  // 100M network status

/**
 * Check if network is connected
 * Return: 1=connected, 0=not connected
 */
unsigned char NE2_IsConnected(void);

/**
 * Get raw state values for debugging
 * Call this to check actual pin levels
 * Usage: unsigned char s0, s1; NE2_GetRawState(&s0, &s1);
 */
void NE2_GetRawState(unsigned char* state0, unsigned char* state1);

#endif  // __NE2_H__

