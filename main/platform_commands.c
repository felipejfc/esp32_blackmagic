/*
 * ESP32-specific platform commands for Black Magic Probe
 *
 * This file provides ESP32-specific GDB monitor commands
 * that extend the upstream command.c functionality.
 */

#include "general.h"
#include "target_internal.h"
#include "gdb_packet.h"
#include "platform.h"
#include "timing.h"

/* External functions from other ESP32 modules */
extern void scan_uart_boot_mode(void);
extern void send_to_uart(int argc, const char **argv);

/*
 * uart_scan command - Scan for STM32 in UART boot mode
 * The target must be in boot mode for this to work.
 */
static bool cmd_uart_scan(target_s *t, int argc, const char **argv)
{
	(void)t;
	(void)argc;
	(void)argv;
	scan_uart_boot_mode();
	return true;
}

/*
 * uart_send command - Send bytes over the TRACESWO_DUMMY_TX pin
 * Usage: mon uart_send <data>
 */
static bool cmd_uart_send(target_s *t, int argc, const char **argv)
{
	(void)t;
	if (argc < 2) {
		gdb_out("Usage: uart_send <data>\n");
		return false;
	}
	send_to_uart(argc, argv);
	gdb_outf("Sending: %s\n", argv[1]);
	platform_delay(500);
	return true;
}

/*
 * Platform-specific command list
 * This is referenced by upstream command.c when PLATFORM_HAS_CUSTOM_COMMANDS is defined
 */
const command_s platform_cmd_list[] = {
	{"uart_scan", cmd_uart_scan, "STM32 UART boot mode scan on TRACESWO pin"},
	{"uart_send", cmd_uart_send, "Send bytes on TRACESWO_DUMMY_TX pin"},
	{NULL, NULL, NULL},
};
