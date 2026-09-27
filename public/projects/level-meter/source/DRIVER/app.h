#ifndef __APP_H__
#define __APP_H__

#include <STC8H.H>

// Application layer functions for main loop processing

/**
 * Process UART4 responses for command queue
 * Receives characters from UART4 interrupt buffer and assembles complete frames
 * Laser module responses end with '>' character
 * Should be called in main loop
 */
void App_ProcessUart4Response(void);

/**
 * Trigger UART batch sending for all UARTs
 * Checks if each UART is idle and has data to send, then triggers batch send
 * Should be called in main loop
 */
void App_TriggerUartSending(void);

/**
 * Process DHT11 sensor reading (non-blocking)
 * Reads DHT11 data every 2.5 seconds and updates MODBUS registers
 * Should be called in main loop
 */
void App_ProcessDHT11(void);

/**
 * Process automatic measurement (non-blocking)
 * Automatically triggers laser measurement at specified interval
 * Uses register 4001 (interval) and 4002 (countdown)
 * @param now: Current system tick from Systick_GetTick()
 * Should be called in main loop
 */
void App_ProcessAutoMeasure(unsigned long now);

/**
 * Process LCD display update (non-blocking)
 * Updates LCD display with current sensor data
 * Should be called in main loop
 */
void App_ProcessLCD(void);

// UART passthrough helper (currently unused; declaration kept for reference)
// void App_ProcessPassthrough(void);

/**
 * Main application loop processing function
 * Processes all non-blocking tasks:
 * - MODBUS RTU requests (UART3)
 * - Key events
 * - Command queue
 * - UART4 responses
 * - DHT11 sensor reading
 * - LCD display update
 * - UART batch sending
 * Should be called in main loop
 */
void App_MainLoop(void);

/**
 * Reset screen activity timer (called when key is pressed or data updated)
 * This will prevent screen timeout and turn on backlight if it was off
 * Should be called when user activity is detected (e.g., key press)
 */
void App_ResetActivityTimer(void);

#if 0
/**
 * Notify that a measurement was successful
 * This will clear manual countdown mode and resume normal auto-measure interval
 * Should be called when a valid measurement result is received
 * Note: Currently not used, cmd_queue.c directly sets auto_measure_manual_countdown_active_flag = 0
 */
void App_NotifyMeasurementSuccess(void);
#endif

// Global flag to clear manual countdown mode when measurement succeeds
extern unsigned char auto_measure_manual_countdown_active_flag;

#if 0
/**
 * Test function: Toggle backlight on/off
 * This is for testing backlight control
 * @param state: 1 = turn on, 0 = turn off
 */
void App_TestBacklight(unsigned char state);
#endif

#endif // __APP_H__

