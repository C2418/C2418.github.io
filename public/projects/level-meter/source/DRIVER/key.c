#include <STC8H.H>
#include "key.h"
#include "systick.h"
#include "uart.h"
#include "laser.h"
#include "cmd_queue.h"
#include <string.h>

// Key data array (non-blocking FSM)
static xdata KeyInfo keyData[KEY_COUNT];

// Read key level (0=pressed, 1=released)
unsigned char readKey(unsigned char id)
{
    switch(id)
    {
        case 0: return KEY1;
        case 1: return KEY2;
        case 2: return KEY3;
        case 3: return KEY4;
        case 4: return KEY5;
        case 5: return KEY6;
        default: return 1;  // Invalid ID, return released
    }
}

/**
 * Initialize key scanning (call once at startup)
 */
void Key_Init(void)
{
    unsigned char i;
    unsigned long now = Systick_GetTick();
    
    // Initialize all keys to IDLE state
    for(i = 0; i < KEY_COUNT; i++)
    {
        keyData[i].state = KS_IDLE;
        keyData[i].ts = now;
        keyData[i].press_ts = 0;
        keyData[i].pending = KP_NONE;
    }
}

/**
 * Update one key state machine (non-blocking, call periodically)
 * @param id: Key ID (0-5)
 * @param now: Current tick (from Systick_GetTick())
 * @return: KeyEvent if event occurred, KEY_NONE otherwise
 */
KeyEvent Key_UpdateOne(unsigned char id, unsigned long now)
{
    KeyInfo *k = &keyData[id];
    unsigned char level = readKey(id);  // 0=pressed, 1=released
    KeyEvent ev = KEY_NONE;
    unsigned long elapsed;
    
    switch(k->state)
    {
        case KS_IDLE:
            if(level == 0)  // Key pressed
            {
                k->state = KS_DEBOUNCE;
                k->ts = now;
            }
            break;
            
        case KS_DEBOUNCE:
            if(level == 0)  // Still pressed
            {
                // Check if debounce time elapsed
                elapsed = (now >= k->ts) ? (now - k->ts) : (0xFFFFFFFFUL - k->ts + now + 1);
                if(elapsed >= KEY_DEBOUNCE_MS)
                {
                    // Debounce complete - first press confirmed
                    k->state = KS_PRESSED;
                    k->press_ts = now;
                    k->pending = KP_NONE;
                }
            }
            else  // Released during debounce (false trigger)
            {
                k->state = KS_IDLE;
                k->pending = KP_NONE;
            }
            break;
            
        case KS_PRESSED:
            if(level == 0)  // Still pressed
            {
                // Check for long press
                elapsed = (now >= k->press_ts) ? (now - k->press_ts) : (0xFFFFFFFFUL - k->press_ts + now + 1);
                if(elapsed >= KEY_LONG_MS)
                {
                    // Long press detected
                    ev = KEY_LONG_PRESS;
                    k->state = KS_IDLE;
                    k->pending = KP_NONE;
                }
            }
            else  // Released - immediately trigger single click
            {
                // Check if it was a long press (shouldn't happen here, but handle it)
                elapsed = (now >= k->press_ts) ? (now - k->press_ts) : (0xFFFFFFFFUL - k->press_ts + now + 1);
                if(elapsed >= KEY_LONG_MS)
                {
                    ev = KEY_LONG_PRESS;
                    k->state = KS_IDLE;
                    k->pending = KP_NONE;
                }
                else
                {
                    // Immediately trigger single click event (no double-click detection)
                    ev = KEY_SINGLE;
                    k->state = KS_IDLE;
                    k->pending = KP_NONE;
                }
            }
            break;
    }
    
    return ev;
}

/**
 * Process all keys and handle events (should be called in main loop)
 * @param now: Current tick (from Systick_GetTick())
 * @param result: Pointer to KeyEventResult to store the result
 * Note: Using pointer parameter instead of return value for C51 compatibility
 */
void Key_Process(unsigned long now, KeyEventResult *result)
{
    unsigned char i;
    KeyEvent ev;
    
    result->event = KEY_NONE;
    result->key_id = 0xFF;
    
    // Scan all keys
    for(i = 0; i < KEY_COUNT; i++)
    {
        ev = Key_UpdateOne(i, now);
        
        if(ev != KEY_NONE)
        {
            result->event = ev;
            result->key_id = i;
        }
    }
}
