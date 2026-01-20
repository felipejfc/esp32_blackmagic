/*
 * This file is part of the Black Magic Debug project.
 *
 * MIT License
 *
 * Copyright (c) 2021 Koen De Vleeschauwer
 * Copyright (c) 2024 ESP32 WiFi port
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * RTT interface for ESP32 WiFi platform
 *
 * Routes RTT data to GDB terminal (via gdb_out) and optionally to WebSocket.
 * - Target to host: rtt_write() sends data to GDB console output
 * - Host to target: rtt_getchar() reads from buffer filled by WebSocket
 */

#include "general.h"
#include "platform.h"
#include "rtt.h"
#include "rtt_if.h"
#include "gdb_packet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "rtt_if";

/* ============================================================================
 * RTT Buffer Configuration
 * ============================================================================ */

#ifndef RTT_UP_BUF_SIZE
#define RTT_UP_BUF_SIZE   (2048U + 8U)
#endif

#ifndef RTT_DOWN_BUF_SIZE
#define RTT_DOWN_BUF_SIZE 256U
#endif

/* ============================================================================
 * Host to Target (Down) Buffer - receives input from WebSocket
 * ============================================================================ */

static char rtt_down_buf[RTT_DOWN_BUF_SIZE];
static volatile uint32_t rtt_down_head = 0;
static volatile uint32_t rtt_down_tail = 0;
static SemaphoreHandle_t rtt_down_mutex = NULL;

/* ============================================================================
 * External WebSocket interface (implemented in web_server.c)
 * ============================================================================ */

/* Send RTT data to WebSocket client */
extern void web_server_send_rtt_data(const uint8_t *data, size_t len);

/* ============================================================================
 * RTT Interface Implementation
 * ============================================================================ */

/* Initialize RTT interface */
int rtt_if_init(void)
{
	if (rtt_down_mutex == NULL) {
		rtt_down_mutex = xSemaphoreCreateMutex();
		if (rtt_down_mutex == NULL) {
			ESP_LOGE(TAG, "Failed to create RTT mutex");
			return -1;
		}
	}

	rtt_down_head = 0;
	rtt_down_tail = 0;

	ESP_LOGI(TAG, "RTT interface initialized");
	return 0;
}

/* Teardown RTT interface */
int rtt_if_exit(void)
{
	if (rtt_down_mutex != NULL) {
		vSemaphoreDelete(rtt_down_mutex);
		rtt_down_mutex = NULL;
	}
	return 0;
}

/*
 * rtt_write - Target to Host
 *
 * Called by RTT core when target sends data (e.g., printf output).
 * We forward this to both GDB console and WebSocket.
 */
uint32_t rtt_write(const uint32_t channel, const char *buf, uint32_t len)
{
	if (len == 0 || buf == NULL)
		return 0;

	/* Only support channel 0 for now */
	if (channel != 0U) {
		ESP_LOGD(TAG, "RTT write to unsupported channel %lu", (unsigned long)channel);
		return len; /* Silently consume */
	}

	/* Send to GDB console - need null-terminated string */
	char tmp[256];
	uint32_t chunk_len = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
	memcpy(tmp, buf, chunk_len);
	tmp[chunk_len] = '\0';
	gdb_out(tmp);

	/* Also send to WebSocket for web UI */
	web_server_send_rtt_data((const uint8_t *)buf, len);

	return len;
}

/*
 * rtt_getchar - Host to Target
 *
 * Called by RTT core when target wants to read input.
 * We read from the buffer filled by WebSocket.
 */
int32_t rtt_getchar(const uint32_t channel)
{
	/* Only support channel 0 */
	if (channel != 0U)
		return -1;

	if (rtt_down_mutex == NULL)
		return -1;

	int32_t retval = -1;

	if (xSemaphoreTake(rtt_down_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
		if (rtt_down_head != rtt_down_tail) {
			retval = (uint8_t)rtt_down_buf[rtt_down_tail];
			rtt_down_tail = (rtt_down_tail + 1U) % RTT_DOWN_BUF_SIZE;
		}
		xSemaphoreGive(rtt_down_mutex);
	}

	return retval;
}

/*
 * rtt_nodata - Check if no host data available
 *
 * Returns true if there's no data from host to target.
 */
bool rtt_nodata(const uint32_t channel)
{
	/* Only support channel 0 */
	if (channel != 0U)
		return true;

	return rtt_down_head == rtt_down_tail;
}

/* ============================================================================
 * WebSocket Input Handler
 * Called by web_server.c when RTT data is received from WebSocket
 * ============================================================================ */

void rtt_if_receive(const uint8_t *data, size_t len)
{
	if (rtt_down_mutex == NULL || data == NULL || len == 0)
		return;

	if (xSemaphoreTake(rtt_down_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
		for (size_t i = 0; i < len; i++) {
			uint32_t next_head = (rtt_down_head + 1U) % RTT_DOWN_BUF_SIZE;
			if (next_head == rtt_down_tail) {
				/* Buffer full - drop remaining data */
				ESP_LOGW(TAG, "RTT down buffer full, dropped %d bytes", (int)(len - i));
				break;
			}
			rtt_down_buf[rtt_down_head] = data[i];
			rtt_down_head = next_head;
		}
		xSemaphoreGive(rtt_down_mutex);
	}
}
