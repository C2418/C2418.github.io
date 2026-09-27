#include <STC8H.H>
#include "delay.h"
#include "uart.h"
#include "NE2.h"              // NE2 module
#include "app.h"
#include "systick.h"
#include "key.h"
#include "cmd_queue.h"
#include "modbus.h"            // MODBUS RTU
#include "laser.h"
#include "dht11.h"             // DHT11 sensor

void main()
{
    // C51 requirement: All variable declarations must be at function start
    unsigned char modbus_slave_addr = 1;  // MODBUS slave address
    
    // ========================================
    // System & peripheral initialization
    // Order is important!
    // ========================================
    Systick_Init();                 // 1ms system tick (Timer2 ISR) - MUST be first  
    // Laser module early initialization (MUST be called before Uart4_Init)           // Wait for MT9700 to stabilize after EN is set high

    UART0_Init_WithInterrupt();     // Debug UART (interrupt FIFO)
    Uart3_Init_WithInterrupt();     // UART3 init for MODBUS/NE2
    
    Laser_Init();                    // Configure laser enable pin (redundant but safe)
    NE2_Init();                      // NE2 network module GPIO setup
    Uart4_Init_WithInterrupt();     // Laser module UART (interrupt FIFO)
    
    Modbus_Init(modbus_slave_addr); // MODBUS RTU slave register setup
    DHT11_Init();                    // DHT11 sensor initialization (P1.0)
    Key_Init();                      // Key FSM init
    CmdQueue_Init();                 // Command queue init (used by keys/laser)
    
    Delay_ms(50);                    // Allow peripherals to settle
    
    // Set initial laser state: OFF (default safe state)
    Laser_Off();                    // Ensure laser is OFF after initialization
    
    // ========================================
    // Main loop: non-blocking APP layer
    // Handles keys, command queue, UART batching (LASER only)
    // ========================================
    while(1)
    {
        App_MainLoop();
        // If needed, enable passthrough for debugging:
        // App_ProcessPassthrough();
    }
}
