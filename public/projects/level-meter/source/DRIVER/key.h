
#ifndef _KEY_H_
#define _KEY_H_

#include <STC8H.H>

// Key pin definitions
sbit KEY1 = P3^2;
sbit KEY2 = P3^3;
sbit KEY3 = P1^1;
sbit KEY4 = P4^7;
sbit KEY5 = P1^6;
sbit KEY6 = P1^7;

// Key timing parameters (all in milliseconds)
#define KEY_SCAN_PERIOD_MS  10     // Scan period (call frequency)
#define KEY_DEBOUNCE_MS     20     // Debounce time
#define KEY_LONG_MS         800    // Long press time
#define KEY_DOUBLE_GAP_MS   350    // Double click gap time
#define KEY_COUNT           6      // Number of keys

// Key event types
typedef enum{
    KEY_NONE = 0,
    KEY_SINGLE,
    KEY_DOUBLE,
    KEY_LONG_PRESS
}KeyEvent;

// Key event result with source key id
typedef struct{
    KeyEvent event;          // Event type
    unsigned char key_id;    // Key index (0-5); 0xFF if none
}KeyEventResult;

// Key state machine states
typedef enum{
    KS_IDLE = 0,
    KS_DEBOUNCE,
    KS_PRESSED,
    KS_WAIT_DOUBLE
}KeyState;

// Key information structure (non-blocking FSM)
typedef enum{
    KP_NONE = 0,
    KP_WAIT_SECOND,
    KP_SECOND_PRESS
}KeyPendingState;

typedef struct{
    volatile KeyState state;            // Current state
    volatile unsigned long ts;          // Timestamp of last state change (ms)
    volatile unsigned long press_ts;    // Timestamp when pressed (ms)
    volatile KeyPendingState pending;   // Pending single/double state
}KeyInfo;

// Initialize key scanning (call once at startup)
void Key_Init(void);

// Update one key state machine (non-blocking, call periodically)
// @param id: Key ID (0-5)
// @param now: Current tick (from Systick_GetTick())
// @return: KeyEvent if event occurred, KEY_NONE otherwise
KeyEvent Key_UpdateOne(unsigned char id, unsigned long now);

// Process all keys and handle events (should be called in main loop)
// @param now: Current tick (from Systick_GetTick())
// @param result: Pointer to KeyEventResult to store the result (event==KEY_NONE if no event)
// Note: Using pointer parameter instead of return value for C51 compatibility
void Key_Process(unsigned long now, KeyEventResult *result);

// Read key level (0=pressed, 1=released)
unsigned char readKey(unsigned char id);

#endif