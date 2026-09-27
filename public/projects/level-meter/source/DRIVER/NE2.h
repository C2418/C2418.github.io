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

// NE2 state helpers are currently unused; keep prototypes commented out
// to avoid accidental use without implementation.
// unsigned char NE2_GetState0(void);
// unsigned char NE2_GetState1(void);

#endif  // __NE2_H__

