
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
#define KEY_CLICK_TIMEOUT_MS 500   // Click/long press timeout (0.5 seconds)
#define KEY_DOUBLE_GAP_MS   300    // Maximum gap between two presses for double click
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
    KS_IDLE = 0,           // 空闲状态
    KS_DEBOUNCE,           // 消抖状态
    KS_FIRST_PRESSED,      // 第一次按下（等待0.5秒判断单击/长按/双击）
    KS_WAIT_RELEASE_LONG,  // 长按识别，等待释放
    KS_WAIT_SECOND,        // 等待第二次按下（双击检测）
    KS_SECOND_DEBOUNCE,    // 第二次按下消抖
    KS_WAIT_RELEASE_DOUBLE // 双击识别，等待释放
}KeyState;

// Key information structure (non-blocking FSM)
typedef struct{
    volatile KeyState state;            // Current state
    volatile unsigned long ts;          // Timestamp of last state change (ms)
    volatile unsigned long first_press_ts; // Timestamp of first press (ms)
    volatile unsigned char press_count; // Press count for double-click detection
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