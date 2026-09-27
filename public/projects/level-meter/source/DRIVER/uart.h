#ifndef __UART0_H__
#define __UART0_H__

#include <STC8H.H>

// MCU frequency
#define FOSC 24000000UL   // 24MHz
#define UART4_RX_BUF_SIZE   64
#define UART4_TX_BUF_SIZE   64
#define UART_TX_BUF_SIZE    256  // Increased from 128 for high-frequency MODBUS TCP
// -------------------------------
// UART0 functions
// -------------------------------

// Initialize UART0, TX only, 9600bps
void UART0_Init(void);

// UART4
void Uart4_Init(void);

// UART3
void Uart3_Init(void);

// Interrupt-based passthrough functions
void UART0_Init_WithInterrupt(void);  // Initialize UART0 with RX interrupt enabled
void Uart4_Init_WithInterrupt(void);  // Initialize UART4 with RX interrupt enabled
void Uart3_Init_WithInterrupt(void);  // Initialize UART3 with RX interrupt enabled
int UART0_GetCharFromBuffer(void);    // Get char from interrupt buffer, returns -1 if empty
int Uart4_GetCharFromBuffer(void);    // Get char from interrupt buffer, returns -1 if empty
int Uart3_GetCharFromBuffer(void);    // Get char from interrupt buffer, returns -1 if empty

// Batch send functions (accumulate and send multiple chars)
void UART0_SendBatch(void);           // Send accumulated chars from buffer
void Uart4_SendBatch(void);           // Send accumulated chars from buffer
void Uart3_SendBatch(void);           // Send accumulated chars from buffer
unsigned char UART0_AddToSendBuffer(char c);  // Add char to send buffer, returns 1 if buffer full
unsigned char Uart4_AddToSendBuffer(char c);  // Add char to send buffer, returns 1 if buffer full
unsigned char Uart3_AddToSendBuffer(char c);  // Add char to send buffer, returns 1 if buffer full
unsigned char UART0_HasDataToSend(void);      // Check if send buffer has data
unsigned char Uart4_HasDataToSend(void);      // Check if send buffer has data
unsigned char Uart3_HasDataToSend(void);      // Check if send buffer has data
unsigned char UART0_IsSending(void);          // Check if UART0 is currently sending
unsigned char Uart4_IsSending(void);           // Check if UART4 is currently sending
unsigned char Uart3_IsSending(void);           // Check if UART3 is currently sending

// Queue string functions (non-blocking, add string to send buffer)
unsigned char UART0_QueueString(char *s);      // Add string to UART0 send buffer, returns 0 on success, 1 if buffer full
unsigned char Uart4_QueueString(char *s);      // Add string to UART4 send buffer, returns 0 on success, 1 if buffer full
unsigned char Uart3_QueueString(char *s);      // Add string to UART3 send buffer, returns 0 on success, 1 if buffer full

#endif  // __UART0_H__

