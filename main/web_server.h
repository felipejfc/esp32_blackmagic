/*
 * Web UI for ESP32 Black Magic Probe
 * Provides HTTP server with WebSocket for UART terminal
 */

#ifndef __WEB_SERVER_H
#define __WEB_SERVER_H

#include <stdint.h>
#include <stdbool.h>

// HTTP server port
#define WEB_SERVER_PORT 80

// Initialize web server (call after WiFi is connected)
void web_server_init(void);

// Send data to all connected WebSocket clients (for UART output)
void web_server_send_uart_data(const uint8_t *data, size_t len);

// Notify UI of target status change
void web_server_notify_target_status(const char *status);

#endif /* __WEB_SERVER_H */
