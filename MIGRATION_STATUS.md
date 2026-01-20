# ESP32 Blackmagic - Upstream Migration Status

This document tracks the migration status of local files to use upstream blackmagic submodule versions.

## Goal

Reduce local code maintenance by using upstream blackmagic files directly where possible, while preserving ESP32-specific functionality.

## Migration Status

### Already Migrated (Using Upstream)

| File | Migration Date | Notes |
|------|---------------|-------|
| `command.c` | 2024-01 | Using upstream with `PLATFORM_HAS_CUSTOM_COMMANDS` for ESP32 extensions |
| `crc32.c` | 2024-01 | Fully upstream, `bmd_crc32()` API |
| `morse.c` | 2024-01 | Fully upstream |
| `rtt.c` | 2024-01 | Fully upstream |
| `hex_utils.c` | 2024-01 | Fully upstream |
| `maths_utils.c` | 2024-01 | Fully upstream |
| `remote.c` | 2024-01 | Fully upstream |
| `exception.c` | 2024-01 | Fully upstream |
| `swdptap.c` | 2024-01 | Fully upstream (was identical) |
| `usb_dfu_stub.c` | 2024-01 | Fully upstream (was identical) |
| `timing.c` | 2024-01 | Fully upstream (better overflow handling) |
| `jtagtap.c` | 2024-01 | Fully upstream (better SWD-to-JTAG transition with ADIv5 selection alert) |
| `gdb_packet.c` | 2026-01 | Using upstream packet API (`gdb_packet_s`, `gdb_packet_receive`) |
| `gdb_main.c` | 2026-01 | Using upstream GDB core (packet helpers, no-ack handling) |
| All `target/*.c` | 2024-01 | Using upstream targets (stm32, nrf, lpc, sam, riscv, etc.) |

### Ready to Migrate (Identical or Trivial)

All trivial/identical migrations have been completed.

### Potential Migrations (Easy to Medium)

None (USB support removed; ESP32 uses WiFi only)

### Hard Migrations (Major Refactoring Required)

None

### ESP32-Specific (Keep Local)

These files have no upstream equivalent or are specifically designed for ESP32:

| File | Purpose |
|------|---------|
| `main.c` | ESP32 FreeRTOS/WiFi entry point |
| `platform.c` | ESP32 GPIO and platform initialization |
| `platform.h` | ESP32 pin definitions, macros |
| `gdb_if.c` | TCP/socket-based GDB interface for ESP32 |
| `web_server.c` | HTTP/WebSocket web UI - unique ESP32 feature |
| `uart_passthrough.c` | UART bridge feature |
| `traceswo.c` | ESP32 SWO capture via UART |
| `traceswodecode.c` | ITM/SWO packet decoder |
| `stubs.c` | Stub implementations for unsupported features |
| `platform_commands.c` | ESP32-specific monitor commands (`uart_scan`, `uart_send`) |
| `swo.h` | Compatibility wrapper for upstream `swo.h` API |
| `stm32flash/*.c` | STM32 UART flash programming support |
| `target/esp32c3.c` | Custom ESP32-C3 target support |

## Compatibility Layer Files

These files provide compatibility between ESP32 and upstream APIs:

| File | Purpose |
|------|---------|
| `swo.h` | Maps upstream `swo_init()`/`swo_deinit()` to local `traceswo_init()` |
| `platform_commands.c` | Provides `platform_cmd_list[]` for `PLATFORM_HAS_CUSTOM_COMMANDS` |
| `stubs.c` | Stubs for `platform_spi_*()`, `onboard_flash_scan()`, `swo_current_mode`, etc. |

## Key API Differences

### Already Handled
- `PC_HOSTED` → `CONFIG_BMDA` (set to 0 in CMakeLists.txt)
- `generic_crc32()` → `bmd_crc32()` (updated in gdb_main.c)
- `PRIx32`/`PRIu32` format specifiers (fixed for RISC-V in platform.h)
- `target_mem_read()` → `target_mem32_read()` (handled in target code)
- `gdb_getpacket()`/`gdb_putpacket()` → `gdb_packet_receive()` and `gdb_put_packet_*()` helpers (ESP32 loops updated for `gdb_packet_s`)

## Build Configuration

In `main/CMakeLists.txt`:
- `ESP32_PLATFORM_SOURCES` - Local ESP32-specific implementations
- `ESP32_CORE_SOURCES` - Local core files (stubs.c, platform_commands.c)
- `UPSTREAM_CORE_UTILS` - Upstream utilities (hex_utils.c, maths_utils.c)
- `UPSTREAM_CORE_SOURCES` - Upstream core (command.c, crc32.c, exception.c, gdb_main.c, gdb_packet.c, morse.c, remote.c, rtt.c)
- `UPSTREAM_TARGET_SOURCES` - All upstream target support

## Cleanup Completed

**Deleted orphaned source files (using upstream):**
- `hex_utils.c`, `maths_utils.c`, `remote.c`, `gdb_hostio.c`

**Deleted redundant headers (identical to or replaced by upstream):**
- `hex_utils.h`, `maths_utils.h`, `remote.h`, `gdb_hostio.h`
- `morse.h`, `serialno.h`, `spi_types.h`, `timing.h`

**Removed local duplicate headers (now using submodule):**
- `align.h`, `buffer_utils.h`, `command.h`, `gdb_if.h`, `gdb_main.h`, `gdb_packet.h`
- `jtagtap.h`, `platform_support.h`, `rtt.h`, `rtt_if.h`, `stdio_newlib.h`, `swd.h`, `target.h`

**Deleted unused USB files (ESP32 uses WiFi, not USB):**
- `usb.c`, `usb_serial.c`, `cdcacm.c`
- `usb.h`, `usb_serial.h`, `cdcacm.h`, `usb_descriptors.h`, `usb_types.h`, `usbuart.h`, `usb_dfu_stub.h`

**Fixed for upstream compatibility:**
- Added `${BM_ROOT}` to include path for `remote.h`
- Fixed `remote_packet_process()` argument order in `gdb_packet.c`

## Next Steps

Migration complete. Ongoing work:
- Keep the upstream submodule updated and re-run the test checklist after bumps
- Re-verify ESP32-specific features (WiFi GDB, web UI, UART passthrough, SWO) after updates

## Testing Checklist

After each migration:
- [ ] `pio run` builds successfully
- [ ] Flash to ESP32-C6 device
- [ ] GDB connection works (port 2345)
- [ ] Target detection works (SWD scan)
- [ ] Basic debug operations (halt, resume, read memory)
- [ ] Web UI accessible
