/*
 * SWO compatibility header for ESP32 Blackmagic Probe
 *
 * This header provides the API expected by upstream command.c
 * while wrapping the local traceswo.c implementation.
 */

#ifndef ESP32_SWO_H
#define ESP32_SWO_H

#include <stdint.h>
#include <stdbool.h>

/* SWO encoding modes (as expected by upstream) */
typedef enum swo_coding {
	swo_none,
	swo_manchester,
	swo_nrz_uart,
} swo_coding_e;

extern swo_coding_e swo_current_mode;

/* Dummy endpoint for compatibility (not used on ESP32 - we use TCP) */
#define SWO_ENDPOINT 5U

/* Default line rate for UART SWO mode */
#define SWO_DEFAULT_BAUD 115200U

/*
 * Initialize SWO capture
 * On ESP32 we only support UART mode, so the mode parameter is ignored.
 */
void swo_init(swo_coding_e swo_mode, uint32_t baudrate, uint32_t itm_stream_bitmask);

/*
 * Deinitialize SWO capture
 * Not implemented on ESP32 - the task keeps running once started.
 */
void swo_deinit(bool deallocate);

#endif /* ESP32_SWO_H */
