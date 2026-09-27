#ifndef __CMD_QUEUE_H__
#define __CMD_QUEUE_H__

#include <STC8H.H>

// Command queue for async UART command-response handling
// Used to avoid blocking in key processing

#define CMD_QUEUE_SIZE 4  // Maximum number of pending commands

// Command states
typedef enum {
    CMD_IDLE = 0,
    CMD_SENT,
    CMD_WAIT_REPLY,
    CMD_DONE,
    CMD_TIMEOUT
} CmdState;

// Command item structure
typedef struct {
    char cmd[24];                    // Command string (e.g., "<mAm>")
    unsigned long timeout_ms;        // Timeout in milliseconds
    unsigned long sent_ts;           // Timestamp when sent (ms)
    CmdState state;                  // Current state
    unsigned char key_id;            // Key ID that triggered this command (for callback)
} CmdItem;

// Initialize command queue
void CmdQueue_Init(void);

// Push a command to queue (non-blocking)
// @param cmd: Command string to send
// @param key_id: Key ID (0-5) that triggered this command
// @param timeout_ms: Timeout in milliseconds
// @return: 0 on success, 1 if queue is full
unsigned char CmdQueue_Push(const char *cmd, unsigned char key_id, unsigned long timeout_ms);

// Process command queue (call in main loop, non-blocking)
// @param now: Current tick (from Systick_GetTick())
void CmdQueue_Process(unsigned long now);

// Handle received response (call when UART4 receives data)
// @param resp: Received response string
void CmdQueue_HandleResponse(const char *resp);

// Check if queue has pending commands (currently unused)
// @return: 1 if queue has commands, 0 if empty
// unsigned char CmdQueue_HasPending(void);

#endif // __CMD_QUEUE_H__

