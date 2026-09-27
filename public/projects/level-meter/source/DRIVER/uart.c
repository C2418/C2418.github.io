#include "uart.h"
#include <STC8H.H>
#include "delay.h"
#include "irq_guard.h"
#include "STC8G_H_Switch.h"

// -------------------------------
// Interrupt-based receive buffers (FIFO)
// -------------------------------
#define UART_RX_BUF_SIZE 64

static unsigned char UART0_RX_Buffer[UART_RX_BUF_SIZE];
static volatile unsigned char UART0_RX_Head = 0;
static volatile unsigned char UART0_RX_Tail = 0;
static volatile unsigned char UART0_RX_Count = 0;

static unsigned char UART4_RX_Buffer[UART_RX_BUF_SIZE];
static volatile unsigned char UART4_RX_Head = 0;
static volatile unsigned char UART4_RX_Tail = 0;
static volatile unsigned char UART4_RX_Count = 0;

static unsigned char UART3_RX_Buffer[UART_RX_BUF_SIZE];
static volatile unsigned char UART3_RX_Head = 0;
static volatile unsigned char UART3_RX_Tail = 0;
static volatile unsigned char UART3_RX_Count = 0;

// Send buffers for batch transmission (interrupt-based)
// UART_TX_BUF_SIZE is defined in uart.h
static unsigned char UART0_TX_Buffer[UART_TX_BUF_SIZE];
static volatile unsigned char UART0_TX_Head = 0;  // Head pointer for reading
static volatile unsigned char UART0_TX_Tail = 0;  // Tail pointer for writing
static volatile unsigned char UART0_TX_Count = 0; // Number of chars in buffer
static volatile unsigned char UART0_TX_Sending = 0;  // Flag: 1 if currently sending

static unsigned char UART4_TX_Buffer[UART_TX_BUF_SIZE];
static volatile unsigned char UART4_TX_Head = 0;  // Head pointer for reading
static volatile unsigned char UART4_TX_Tail = 0;  // Tail pointer for writing
static volatile unsigned char UART4_TX_Count = 0; // Number of chars in buffer
static volatile unsigned char UART4_TX_Sending = 0;  // Flag: 1 if currently sending

static unsigned char UART3_TX_Buffer[UART_TX_BUF_SIZE];
static volatile unsigned char UART3_TX_Head = 0;  // Head pointer for reading
static volatile unsigned char UART3_TX_Tail = 0;  // Tail pointer for writing
static volatile unsigned char UART3_TX_Count = 0; // Number of chars in buffer
static volatile unsigned char UART3_TX_Sending = 0;  // Flag: 1 if currently sending

// Overflow flag for UART4 (set when RX buffer is full, used to reset FIFO state)
// Using unsigned char instead of bit for C51 compatibility with volatile
static volatile unsigned char uart4_rx_overflow = 0;

// -------------------------------
// UART0 Init, TX only, 9600bps
// -------------------------------

void UART0_Init(void)   // 9600bps @ 24.000MHz
{
    PCON &= 0x7F;        
    SCON = 0x50;         
    AUXR |= 0x40;        
    AUXR &= 0xFE;       
    
    TMOD &= 0x0F;        
    TMOD |= 0x20;        
    
    TL1 = 0xB2;          
    TH1 = 0xB2;          
    
    ET1 = 0;             
    TR1 = 1;             

    TI = 1;              
}





//laser uart
// -------------------------------
// UART4 Init, TXD4=P0.3, RXD4=P0.2, 9600bps @24MHz
// Reference STC official example: UART pins should be configured as quasi-bidirectional mode
// -------------------------------
void Uart4_Init(void)
{
    // Configure UART4 pin mapping to P0.2(RXD4) and P0.3(TXD4)
    UART4_SW(UART4_SW_P02_P03);
    
    // GPIO pin configuration as quasi-bidirectional mode (reference official example)
    // P0.2 (RXD4) and P0.3 (TXD4) both configured as quasi-bidirectional mode
    // Quasi-bidirectional mode: P0M0=0, P0M1=0
    P0M0 &= ~0x0C;  // Clear P0.2 and P0.3 P0M0 bits (bit2 and bit3)
    P0M1 &= ~0x0C;  // Clear P0.2 and P0.3 P0M1 bits (bit2 and bit3)

    // Explicitly set required bits for clarity
    S4CON = 0x00;
    S4CON |= 0x10;   // REN4 = 1 (enable RX)
    S4CON |= 0x40;   // Enable UART4 (SM4/EN)

    T4T3M |= 0x20;   
    T4L = 0x8F;      
    T4H = 0xFD;
    T4T3M |= 0x80;   
}




//


// -------------------------------
// UART3 Init, 9600bps @24.000MHz
// WARNING: UART3 TX/RX functions are hardware fixed, cannot be swapped by software
// If TX and RX are reversed in hardware connection, need to swap hardware connection
// Current configuration: P0.0=RXD3, P0.1=TXD3 (standard hardware configuration)
// If hardware is reversed, swap external connection wires, or try P5.0/P5.1 pin mapping
// -------------------------------
void Uart3_Init(void)
{
    // UART3 -> P0.0 / P0.1
    P_SW2 = (P_SW2 & 0xFD) | (0x00 << 1);

    // P0.0 RXD3, P0.1 TXD3: Quasi-bidirectional
    P0M0 &= ~0x03;
    P0M1 &= ~0x03;

    // UART3: 8bit, Timer3, RX enabled
    S3CON = 0x50;

    // Configure Timer3 for baud rate generation (9600bps @ 24MHz)
    // Reference: STC8G_H_UART.c line 173-174
    T3L = 0x8F;         // Timer3 low byte (same as Timer4 for 9600bps @ 24MHz)
    T3H = 0xFD;         // Timer3 high byte
    T4T3M &= 0xf0;      // Clear Timer3 bits (bit0-3) to avoid affecting Timer4
    T4T3M |= 0x0a;      // Timer3: As Timer, 1T mode (bit1=1), Start timer3 (bit3=1)
}




// -------------------------------
// Interrupt-based UART initialization and handlers
// -------------------------------

// Initialize UART0 with RX interrupt enabled
void UART0_Init_WithInterrupt(void)
{
    unsigned char _ea = IRQ_Save();  // Guard during init to avoid early ISR entry
    UART0_Init();  // Use existing initialization
    
    // Clear RX buffer pointers
    UART0_RX_Head = 0;
    UART0_RX_Tail = 0;
    UART0_RX_Count = 0;
    
    // Clear TX buffer pointers
    UART0_TX_Head = 0;
    UART0_TX_Tail = 0;
    UART0_TX_Count = 0;
    UART0_TX_Sending = 0;
    
    // Enable UART0 RX and TX interrupt
    ES = 1;  // Enable serial port interrupt (both RX and TX)
    IRQ_Restore(_ea);  // Restore global interrupt state
}

// Initialize UART4 with RX interrupt enabled
void Uart4_Init_WithInterrupt(void)
{
    unsigned char _ea = IRQ_Save();  // Guard during init to avoid early ISR entry
    Uart4_Init();  // Use existing initialization
    
    // Clear RX buffer pointers
    UART4_RX_Head = 0;
    UART4_RX_Tail = 0;
    UART4_RX_Count = 0;
    
    // Clear TX buffer pointers
    UART4_TX_Head = 0;
    UART4_TX_Tail = 0;
    UART4_TX_Count = 0;
    UART4_TX_Sending = 0;
    
    // Enable UART4 RX and TX interrupt
    IE2 |= 0x10;  // Enable UART4 interrupt (ES4, bit4)
    IRQ_Restore(_ea);  // Restore global interrupt state
}

// Initialize UART3 with RX interrupt enabled
void Uart3_Init_WithInterrupt(void)
{
    unsigned char _ea;  // C51: all variables must be declared at function start
    volatile unsigned char dummy;  // For draining S3BUF if needed
    
    Uart3_Init();  // Use existing initialization
    
    // Disable interrupts for critical section
    _ea = IRQ_Save();  // Guard during init to avoid early ISR entry
    
    // Clear RX buffer pointers
    UART3_RX_Head = 0;
    UART3_RX_Tail = 0;
    UART3_RX_Count = 0;
    
    // Clear TX buffer pointers
    UART3_TX_Head = 0;
    UART3_TX_Tail = 0;
    UART3_TX_Count = 0;
    UART3_TX_Sending = 0;
    
    // Clear any pending UART3 RX interrupt flag to avoid immediate interrupt storm
    if(S3CON & 0x01)  // RI3 flag
    {
        S3CON &= ~0x01;  // Clear RI3
        // Drain any data in S3BUF to prevent immediate interrupt
        dummy = S3BUF;
        (void)dummy;  // Suppress unused variable warning
    }
    
    // Enable UART3 RX and TX interrupt
    IE2 |= 0x08;  // Enable UART3 interrupt (ES3, bit3)
    
    // Restore global interrupt state
    IRQ_Restore(_ea);
}

// Get character from UART0 interrupt buffer
// NOTE: Not used in current project; kept under #if 0 to avoid L16 warning.
#if 0
int UART0_GetCharFromBuffer(void)
{
    unsigned char idx;
    unsigned char _ea = IRQ_Save();
    if(UART0_RX_Count == 0)
    {
        IRQ_Restore(_ea);
        return -1;
    }
    idx = UART0_RX_Tail;
    UART0_RX_Tail = (UART0_RX_Tail + 1) % UART_RX_BUF_SIZE;
    UART0_RX_Count--;
    IRQ_Restore(_ea);
    return UART0_RX_Buffer[idx];
}
#endif

// Get character from UART4 interrupt buffer
int Uart4_GetCharFromBuffer(void)
{
    unsigned char idx;
    unsigned char _ea = IRQ_Save();
    // If an RX overflow was detected, drop the partial frame and reset FIFO state
    // so upper layers don't stay in a permanent "half-frame" condition.
    if(uart4_rx_overflow)
    {
        UART4_RX_Count = 0;
        UART4_RX_Head = 0;
        UART4_RX_Tail = 0;
        uart4_rx_overflow = 0;
    }
    if(UART4_RX_Count == 0)
    {
        IRQ_Restore(_ea);
        return -1;         
    }
    idx = UART4_RX_Tail;
    UART4_RX_Tail = (UART4_RX_Tail + 1) % UART_RX_BUF_SIZE;
    UART4_RX_Count--;
    IRQ_Restore(_ea);
    return UART4_RX_Buffer[idx];
}

int Uart3_GetCharFromBuffer(void)
{
    unsigned char idx;
    unsigned char _ea = IRQ_Save();
    if(UART3_RX_Count == 0)
    {
        IRQ_Restore(_ea);
        return -1;
    }
    idx = UART3_RX_Tail;
    UART3_RX_Tail = (UART3_RX_Tail + 1) % UART_RX_BUF_SIZE;
    UART3_RX_Count--;
    IRQ_Restore(_ea);
    return UART3_RX_Buffer[idx];
}

// UART0 interrupt service routine
void UART0_ISR(void) interrupt 4
{
    if(RI)
    {
        unsigned char ch = SBUF;  // Read first
        RI = 0;                   // Then clear flag
        if(UART0_RX_Count < UART_RX_BUF_SIZE)
        {
            UART0_RX_Buffer[UART0_RX_Head] = ch;
            UART0_RX_Head++;
            if(UART0_RX_Head >= UART_RX_BUF_SIZE)
            {
                UART0_RX_Head = 0;
            }
            UART0_RX_Count++;
        }
        // Buffer overflow: data lost
    }
    if(TI)  // Transmit interrupt
    {
        if(UART0_TX_Count > 0)  // More data to send
        {
            SBUF = UART0_TX_Buffer[UART0_TX_Head];
            UART0_TX_Head++;
            if(UART0_TX_Head >= UART_TX_BUF_SIZE)
            {
                UART0_TX_Head = 0;
            }
            UART0_TX_Count--;
        }
        else
        {
            UART0_TX_Sending = 0;  // Sending complete
        }
        TI = 0;  // Clear TI flag last (correct order for STC8, avoid race condition)
    }
}

// UART4 interrupt service routine
void UART4_ISR(void) interrupt 18
{
    unsigned char idx;
    if(S4CON & 0x01)  // RI4 flag
{
        unsigned char ch = S4BUF;  // Read first
        S4CON &= ~0x01;            // Then clear
        if(UART4_RX_Count < UART_RX_BUF_SIZE)
        {
            UART4_RX_Buffer[UART4_RX_Head] = ch;
            UART4_RX_Head++;
            if(UART4_RX_Head >= UART_RX_BUF_SIZE)
    {
                UART4_RX_Head = 0;
    }
            UART4_RX_Count++;
        }
        else
        {
            uart4_rx_overflow = 1;  // Buffer overflow flag for debugging
        }
    }
    if(S4CON & 0x02)  // TI4 flag
    {
        if(UART4_TX_Count > 0)  // More data to send
        {
            idx = UART4_TX_Head;
            S4BUF = UART4_TX_Buffer[idx];
            UART4_TX_Head++;
            if(UART4_TX_Head >= UART_TX_BUF_SIZE)
            {
                UART4_TX_Head = 0;
            }
            UART4_TX_Count--;
        }
        else
        {
            UART4_TX_Sending = 0;  // Sending complete
        }
        S4CON &= ~0x02;  // Clear TI4 flag last (correct order for STC8, avoid race condition)
    }
}

// UART3 interrupt service routine
void UART3_ISR(void) interrupt 17
{
    unsigned char idx;
    if(S3CON & 0x01)  // RI3 flag
    {
        unsigned char ch = S3BUF;  // Read first
        S3CON &= ~0x01;            // Then clear
        if(UART3_RX_Count < UART_RX_BUF_SIZE)
        {
            UART3_RX_Buffer[UART3_RX_Head] = ch;
            UART3_RX_Head++;
            if(UART3_RX_Head >= UART_RX_BUF_SIZE)
        {
                UART3_RX_Head = 0;
        }
            UART3_RX_Count++;
        }
        else
        {
            // Buffer overflow: data lost
        }
    }
    if(S3CON & 0x02)  // TI3 flag
    {
        if(UART3_TX_Count > 0)  // More data to send
        {
            idx = UART3_TX_Head;
            S3BUF = UART3_TX_Buffer[idx];
            UART3_TX_Head++;
            if(UART3_TX_Head >= UART_TX_BUF_SIZE)
            {
                UART3_TX_Head = 0;
            }
            UART3_TX_Count--;
        }
        else
        {
            UART3_TX_Sending = 0;  // Sending complete
        }
        S3CON &= ~0x02;  // Clear TI3 flag last (correct order for STC8, avoid race condition)
    }
}

// -------------------------------
// Batch send functions
// -------------------------------

// Add character to UART0 send buffer (FIFO queue)
unsigned char UART0_AddToSendBuffer(char c)
{
    unsigned char next_tail;
    unsigned char _ea = IRQ_Save();  // Critical section: protect Tail and Count from ISR
    if(UART0_TX_Count >= UART_TX_BUF_SIZE)
    {
        IRQ_Restore(_ea);
        return 1;  // Buffer full
    }
    next_tail = UART0_TX_Tail + 1;
    if(next_tail >= UART_TX_BUF_SIZE)
    {
        next_tail = 0;
    }
    UART0_TX_Buffer[UART0_TX_Tail] = c;
    UART0_TX_Tail = next_tail;
    UART0_TX_Count++;
    IRQ_Restore(_ea);  // End critical section
    return 0;  // Success
}

// Add character to UART4 send buffer (FIFO queue)
unsigned char Uart4_AddToSendBuffer(char c)
{
    unsigned char next_tail;
    unsigned char _ea = IRQ_Save();  // Critical section: protect Tail and Count from ISR
    if(UART4_TX_Count >= UART_TX_BUF_SIZE)
    {
        IRQ_Restore(_ea);
        return 1;  // Buffer full
    }
    next_tail = UART4_TX_Tail + 1;
    if(next_tail >= UART_TX_BUF_SIZE)
    {
        next_tail = 0;
    }
    UART4_TX_Buffer[UART4_TX_Tail] = c;
    UART4_TX_Tail = next_tail;
    UART4_TX_Count++;
    IRQ_Restore(_ea);  // End critical section
    return 0;  // Success
}

// Add character to UART3 send buffer (FIFO queue)
unsigned char Uart3_AddToSendBuffer(char c)
{
    unsigned char next_tail;
    unsigned char _ea = IRQ_Save();  // Critical section: protect Tail and Count from ISR
    if(UART3_TX_Count >= UART_TX_BUF_SIZE)
    {
        IRQ_Restore(_ea);
        return 1;  // Buffer full
    }
    next_tail = UART3_TX_Tail + 1;
    if(next_tail >= UART_TX_BUF_SIZE)
    {
        next_tail = 0;
    }
    UART3_TX_Buffer[UART3_TX_Tail] = c;
    UART3_TX_Tail = next_tail;
    UART3_TX_Count++;
    IRQ_Restore(_ea);  // End critical section
    return 0;  // Success
}

// Start interrupt-based batch send for UART0
void UART0_SendBatch(void)
{
    if(UART0_TX_Count == 0)
    {
        return;  // Nothing to send
    }
    if(UART0_TX_Sending == 0)  // Not currently sending
    {
        unsigned char ch;
        unsigned char _ea = IRQ_Save();  // Protect head/count/Sending while priming first byte
        ch = UART0_TX_Buffer[UART0_TX_Head];
        UART0_TX_Head = (UART0_TX_Head + 1) % UART_TX_BUF_SIZE;
        UART0_TX_Count--;
        UART0_TX_Sending = 1;
        TI = 0;        // Clear TI first to avoid race on STC8
        SBUF = ch;     // Prime first byte
        IRQ_Restore(_ea);
    }
}

// Start interrupt-based batch send for UART4
void Uart4_SendBatch(void)
{
    if(UART4_TX_Count == 0)
    {
        return;  // Nothing to send
    }
    if(UART4_TX_Sending == 0)  // Not currently sending
    {
        unsigned char ch;
        unsigned char _ea = IRQ_Save();  // Protect head/count/Sending while priming first byte
        ch = UART4_TX_Buffer[UART4_TX_Head];
        UART4_TX_Head = (UART4_TX_Head + 1) % UART_TX_BUF_SIZE;
        UART4_TX_Count--;
        UART4_TX_Sending = 1;
        // Clear TI4 first, then write S4BUF to avoid the race condition where
        // TI4 was already set before priming the first byte (same pattern as UART0)
        S4CON &= ~0x02;
        S4BUF = ch;
        IRQ_Restore(_ea);
    }
}

// Start interrupt-based batch send for UART3
void Uart3_SendBatch(void)
{
    if(UART3_TX_Count == 0)
{
        return;  // Nothing to send
    }
    if(UART3_TX_Sending == 0)  // Not currently sending
    {
        unsigned char ch;
        unsigned char _ea = IRQ_Save();  // Protect head/count/Sending while priming first byte
        ch = UART3_TX_Buffer[UART3_TX_Head];
        UART3_TX_Head = (UART3_TX_Head + 1) % UART_TX_BUF_SIZE;
        UART3_TX_Count--;
        UART3_TX_Sending = 1;
        // Clear TI3 first, then write S3BUF to avoid the race condition where
        // TI3 was already set before priming the first byte (same pattern as UART0)
        S3CON &= ~0x02;
        S3BUF = ch;
        IRQ_Restore(_ea);
    }
}

// Check if UART0 send buffer has data
unsigned char UART0_HasDataToSend(void)
{
    return (UART0_TX_Count > 0) ? 1 : 0;
}

// Check if UART4 send buffer has data
unsigned char Uart4_HasDataToSend(void)
{
    return (UART4_TX_Count > 0) ? 1 : 0;
}

// Check if UART3 send buffer has data
unsigned char Uart3_HasDataToSend(void)
{
    return (UART3_TX_Count > 0) ? 1 : 0;
}

// Check if UART0 is currently sending
unsigned char UART0_IsSending(void)
{
    return UART0_TX_Sending;
}

// Check if UART4 is currently sending
unsigned char Uart4_IsSending(void)
{
    return UART4_TX_Sending;
    }

// Check if UART3 is currently sending
unsigned char Uart3_IsSending(void)
{
    return UART3_TX_Sending;
}

// -------------------------------
// Queue string functions (non-blocking)
// -------------------------------

// Add string to UART0 send buffer (non-blocking)
unsigned char UART0_QueueString(char *s)
{
    unsigned char count = 0;
    if(s == 0)
    {
        return 1;  // Invalid pointer
    }
    // Non-blocking: try to add as many characters as possible, but don't wait
    while(*s && count < 60)  // Limit to prevent long strings from blocking
    {
        if(UART0_AddToSendBuffer(*s) != 0)
        {
            // Buffer full, trigger send and try once more
            if(UART0_IsSending() == 0 && UART0_HasDataToSend())
            {
                UART0_SendBatch();
            }
            // Try once more, if still full, give up (non-blocking)
            if(UART0_AddToSendBuffer(*s) != 0)
            {
                return 1;  // Buffer still full, stop
            }
        }
        s++;
        count++;
    }
    return 0;  // Success
}

// Add string to UART4 send buffer (non-blocking)
unsigned char Uart4_QueueString(char *s)
{
    if(s == 0)
    {
        return 1;  // Invalid pointer
    }
    while(*s)
    {
        if(Uart4_AddToSendBuffer(*s) != 0)
        {
            return 1;  // Buffer full
        }
        s++;
    }
    return 0;  // Success
}

// Add string to UART3 send buffer (non-blocking)
// NOTE: Not used in current project; kept under #if 0 to avoid L16 warning.
#if 0
unsigned char Uart3_QueueString(char *s)
{
    if(s == 0)
    {
        return 1;  // Invalid pointer
    }
    while(*s)
    {
        if(Uart3_AddToSendBuffer(*s) != 0)
        {
            return 1;  // Buffer full
        }
        s++;
    }
    return 0;  // Success
}
#endif

