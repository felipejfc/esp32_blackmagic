/*
 * UART passthrough for ESP32 Black Magic Probe
 * Provides a TCP socket that bridges to target's UART
 */

#include "platform.h"

#ifdef PLATFORM_HAS_UART_PASSTHROUGH

#include "uart_passthrough.h"
#include "web_server.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "uart_passthrough";

#define UART_BUF_SIZE 1024
#define TCP_BUF_SIZE  1024

static uint32_t current_baud = TARGET_UART_BAUD;
static int client_socket = -1;
static volatile bool uart_initialized = false;

static void uart_hw_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = current_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(TARGET_UART_PORT, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(TARGET_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(TARGET_UART_PORT, TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uart_initialized = true;
    ESP_LOGI(TAG, "UART%d initialized: TX=GPIO%d, RX=GPIO%d, baud=%lu",
             TARGET_UART_PORT, TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, current_baud);
}

void uart_passthrough_set_baud(uint32_t baud)
{
    current_baud = baud;
    if (uart_initialized) {
        uart_set_baudrate(TARGET_UART_PORT, baud);
        ESP_LOGI(TAG, "Baud rate changed to %lu", baud);
    }
}

uint32_t uart_passthrough_get_baud(void)
{
    return current_baud;
}

// Task to read from UART and send to TCP client and Web UI
static void uart_to_tcp_task(void *pvParameters)
{
    uint8_t *data = malloc(UART_BUF_SIZE);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate UART buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // Always read from UART, even without TCP client (for Web UI)
        int len = uart_read_bytes(TARGET_UART_PORT, data, UART_BUF_SIZE, pdMS_TO_TICKS(20));
        if (len > 0) {
            // Send to TCP client if connected
            if (client_socket >= 0) {
                int sent = send(client_socket, data, len, 0);
                if (sent < 0) {
                    ESP_LOGD(TAG, "TCP send failed, client may have disconnected");
                }
            }
            // Always send to Web UI
            web_server_send_uart_data(data, len);
        }
    }

    free(data);
    vTaskDelete(NULL);
}

// Task to read from TCP and send to UART
static void tcp_to_uart_task(void *pvParameters)
{
    uint8_t *data = malloc(TCP_BUF_SIZE);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate TCP buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (client_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int len = recv(client_socket, data, TCP_BUF_SIZE, 0);
        if (len > 0) {
            uart_write_bytes(TARGET_UART_PORT, data, len);
        } else if (len == 0) {
            // Connection closed
            ESP_LOGI(TAG, "Client disconnected");
            close(client_socket);
            client_socket = -1;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available, yield to other tasks
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            close(client_socket);
            client_socket = -1;
        }
    }

    free(data);
    vTaskDelete(NULL);
}

// Main TCP server task
static void uart_tcp_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(UART_PASSTHROUGH_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART passthrough TCP server listening on port %d", UART_PASSTHROUGH_PORT);

    while (1) {
        ESP_LOGI(TAG, "Waiting for UART client connection...");

        int new_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (new_sock < 0) {
            ESP_LOGE(TAG, "Accept failed: errno %d", errno);
            continue;
        }

        // Close existing client if any
        if (client_socket >= 0) {
            ESP_LOGI(TAG, "Closing previous client connection");
            close(client_socket);
        }

        client_socket = new_sock;

        char addr_str[16];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "UART client connected from %s", addr_str);

        // Set socket to non-blocking for recv in tcp_to_uart_task
        int flags = fcntl(client_socket, F_GETFL, 0);
        fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
    }

    close(listen_sock);
    vTaskDelete(NULL);
}

void uart_passthrough_init(void)
{
    // Initialize UART hardware
    uart_hw_init();

    // Create tasks for bidirectional data transfer
    xTaskCreate(uart_tcp_server_task, "uart_tcp_srv", 4096, NULL, 5, NULL);
    xTaskCreate(uart_to_tcp_task, "uart_to_tcp", 4096, NULL, 6, NULL);
    xTaskCreate(tcp_to_uart_task, "tcp_to_uart", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "UART passthrough initialized on port %d", UART_PASSTHROUGH_PORT);
}

#endif /* PLATFORM_HAS_UART_PASSTHROUGH */
