/*
 * UART passthrough for ESP32 Black Magic Probe
 * Provides a TCP socket that bridges to target's UART
 */

#ifndef __UART_PASSTHROUGH_H
#define __UART_PASSTHROUGH_H

#include <stdint.h>
#include <stdbool.h>

// Default TCP port for UART passthrough (GDB is on 2345)
#define UART_PASSTHROUGH_PORT 2346

// Initialize UART passthrough (call after WiFi is connected)
void uart_passthrough_init(void);

// Set UART baud rate (can be changed at runtime)
void uart_passthrough_set_baud(uint32_t baud);

// Get current baud rate
uint32_t uart_passthrough_get_baud(void);

#endif /* __UART_PASSTHROUGH_H */
