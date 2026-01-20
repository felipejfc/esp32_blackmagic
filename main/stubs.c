/*
 * Stub implementations for upstream blackmagic functions not needed on ESP32
 *
 * This file provides empty implementations for functions that are:
 * 1. Platform-specific and not used on ESP32
 * 2. Required by upstream code but not applicable to our use case
 */

#include "general.h"
#include "swo.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * SWO mode variable - used by upstream command.c
 */
swo_coding_e swo_current_mode = swo_nrz_uart;

/* Forward declaration of traceswo_init from traceswo.c */
void traceswo_init(uint32_t baudrate, uint32_t swo_chan_bitmask);

/*
 * SWO init/deinit - wraps local traceswo implementation
 */
void swo_init(swo_coding_e swo_mode, uint32_t baudrate, uint32_t itm_stream_bitmask)
{
    (void)swo_mode; /* ESP32 only supports UART mode */
    swo_current_mode = swo_mode;
    traceswo_init(baudrate, itm_stream_bitmask);
}

void swo_deinit(bool deallocate)
{
    (void)deallocate;
    swo_current_mode = swo_none;
    /* Not fully implemented - task keeps running once started */
}

/*
 * SPI platform functions - not implemented on ESP32 port
 * These are used for direct SPI flash access on some platforms
 */
#include "spi_types.h"

bool platform_spi_chip_select(uint8_t device_select)
{
    (void)device_select;
    /* Not implemented - ESP32 port uses memory-mapped flash via target debug interface */
    return false;
}

uint8_t platform_spi_xfer(spi_bus_e bus, uint8_t value)
{
    (void)bus;
    (void)value;
    /* Not implemented */
    return 0;
}

bool platform_spi_init(spi_bus_e bus)
{
    (void)bus;
    /* Not implemented */
    return false;
}

bool platform_spi_deinit(spi_bus_e bus)
{
    (void)bus;
    /* Not implemented */
    return false;
}

/*
 * Debug serial output - forward to WebSocket for semihosting support
 */
extern void web_server_send_rtt_data(const uint8_t *data, size_t len);

void debug_serial_send_stdout(const uint8_t *data, size_t len)
{
    /* Forward semihosting stdout to WebSocket as RTT-style data */
    web_server_send_rtt_data(data, len);
}

/*
 * On-board flash scan - not implemented on ESP32 port
 * This is used by spi_scan command in upstream command.c
 */
bool onboard_flash_scan(void)
{
    /* Not implemented - ESP32 doesn't have on-board SPI flash access via debug interface */
    return false;
}
