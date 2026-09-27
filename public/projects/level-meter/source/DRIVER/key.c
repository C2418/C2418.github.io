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
        keyData[i].first_press_ts = 0;
        keyData[i].press_count = 0;
    }
}

/**
 * Update one key state machine (non-blocking, call periodically)
 * 按键识别逻辑：
 * - 单击：0.5秒内按下并释放（只按一次）
 * - 长按：0.5秒后还在按下状态，释放后触发
 * - 双击：0.5秒内按下两次（第二次可按可不按），释放后触发
 * 
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
    unsigned long elapsed_from_first;  // C51 requirement: declare at function start
    
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
            elapsed = (now >= k->ts) ? (now - k->ts) : (0xFFFFFFFFUL - k->ts + now + 1);
            
            if(level == 0)  // Still pressed
            {
                if(elapsed >= KEY_DEBOUNCE_MS)
                {
                    // Debounce complete - first press confirmed
                    k->state = KS_FIRST_PRESSED;
                    k->first_press_ts = now;
                    k->press_count = 1;
                }
            }
            else  // Released during debounce (false trigger)
            {
                k->state = KS_IDLE;
                k->press_count = 0;
            }
            break;
            
        case KS_FIRST_PRESSED:
            elapsed = (now >= k->first_press_ts) ? (now - k->first_press_ts) : (0xFFFFFFFFUL - k->first_press_ts + now + 1);
            
            if(elapsed >= KEY_CLICK_TIMEOUT_MS)  // 0.5秒超时
            {
                if(level == 0)  // Still pressed after 0.5s -> Long press
                {
                    // 长按识别：0.5秒后还按着，等待释放
                    k->state = KS_WAIT_RELEASE_LONG;
                }
                else  // Released before timeout -> Should not happen (handled below)
                {
                    // This case is handled in the "else" branch below
                }
            }
            else  // Within 0.5s timeout
            {
                if(level == 1)  // Released within 0.5s
                {
                    // 可能是单击或双击的第一次按下
                    // 等待看是否有第二次按下
                    k->state = KS_WAIT_SECOND;
                    k->ts = now;  // Record release time
                }
                // If still pressed, continue waiting for timeout
            }
            break;
            
        case KS_WAIT_RELEASE_LONG:
            // 长按已识别，等待释放后通知
            if(level == 1)  // Released
            {
                ev = KEY_LONG_PRESS;
                k->state = KS_IDLE;
                k->press_count = 0;
            }
            break;
            
        case KS_WAIT_SECOND:
            // 等待第二次按下（双击检测）
            elapsed = (now >= k->ts) ? (now - k->ts) : (0xFFFFFFFFUL - k->ts + now + 1);
            
            if(level == 0)  // Second press detected
            {
                // Check if within double-click gap time
                if(elapsed <= KEY_DOUBLE_GAP_MS)
                {
                    // Second press within gap -> Double click
                    k->state = KS_SECOND_DEBOUNCE;
                    k->ts = now;
                }
                else
                {
                    // Second press too late -> Treat first as single click, start new sequence
                    ev = KEY_SINGLE;
                    k->state = KS_DEBOUNCE;
                    k->ts = now;
                }
            }
            else  // No second press
            {
                // Check if first press timeout expired (0.5s from first press)
                elapsed_from_first = (now >= k->first_press_ts) ? 
                    (now - k->first_press_ts) : (0xFFFFFFFFUL - k->first_press_ts + now + 1);
                
                if(elapsed_from_first >= KEY_CLICK_TIMEOUT_MS)
                {
                    // 0.5秒内只按了一次 -> Single click
                    ev = KEY_SINGLE;
                    k->state = KS_IDLE;
                    k->press_count = 0;
                }
            }
            break;
            
        case KS_SECOND_DEBOUNCE:
            elapsed = (now >= k->ts) ? (now - k->ts) : (0xFFFFFFFFUL - k->ts + now + 1);
            
            if(level == 0)  // Still pressed
            {
                if(elapsed >= KEY_DEBOUNCE_MS)
                {
                    // Second press debounce complete -> Double click confirmed
                    k->press_count = 2;
                    
                    // Check if we're still within 0.5s from first press
                    elapsed_from_first = (now >= k->first_press_ts) ? 
                        (now - k->first_press_ts) : (0xFFFFFFFFUL - k->first_press_ts + now + 1);
                    
                    if(elapsed_from_first < KEY_CLICK_TIMEOUT_MS)
                    {
                        // Double click within 0.5s -> Wait for release
                        k->state = KS_WAIT_RELEASE_DOUBLE;
                    }
                    else
                    {
                        // Exceeded 0.5s -> Treat as new press
                        k->state = KS_FIRST_PRESSED;
                        k->first_press_ts = now;
                        k->press_count = 1;
                    }
                }
            }
            else  // Released during second debounce
            {
                // False trigger, go back to waiting for second press
                k->state = KS_WAIT_SECOND;
            }
            break;
            
        case KS_WAIT_RELEASE_DOUBLE:
            // 双击已识别，等待释放后通知
            if(level == 1)  // Released
            {
                ev = KEY_DOUBLE;
                k->state = KS_IDLE;
                k->press_count = 0;
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
