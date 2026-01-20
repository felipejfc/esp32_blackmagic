# ESP32 Blackmagic Probe - CMakeLists.txt
# Uses upstream blackmagic submodule for target support

set(BM_ROOT ${CMAKE_SOURCE_DIR}/blackmagic/src)
set(BM_TARGET ${BM_ROOT}/target)

# =============================================================================
# ESP32-specific sources (local implementations)
# =============================================================================
set(ESP32_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gdb_if.c
    ${CMAKE_CURRENT_SOURCE_DIR}/uart_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/web_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/traceswo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/traceswodecode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rtt_if.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stm32flash/stm32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stm32flash/esp32_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stm32flash/dev_table.c
)

# Upstream platform sources (using upstream directly)
set(UPSTREAM_PLATFORM_SOURCES
    ${BM_ROOT}/platforms/common/swdptap.c
    ${BM_ROOT}/platforms/common/jtagtap.c
    ${BM_ROOT}/platforms/common/usb_dfu_stub.c
    ${BM_ROOT}/timing.c
)

# These could potentially use upstream but keeping local for now
set(ESP32_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/gdb_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gdb_packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_commands.c
)

# Use upstream hex_utils.c and maths_utils.c (has functions needed by remote.c and swdptap.c)
set(UPSTREAM_CORE_UTILS
    ${BM_ROOT}/hex_utils.c
    ${BM_ROOT}/maths_utils.c
)

# Upstream core sources (not target-specific)
set(UPSTREAM_CORE_SOURCES
    ${BM_ROOT}/command.c
    ${BM_ROOT}/crc32.c
    ${BM_ROOT}/exception.c
    ${BM_ROOT}/morse.c
    ${BM_ROOT}/remote.c
    ${BM_ROOT}/rtt.c
)

# =============================================================================
# Custom ESP32 target (not in upstream)
# =============================================================================
set(CUSTOM_TARGET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/target/esp32c3.c
)

# =============================================================================
# Upstream blackmagic target sources from submodule
# =============================================================================

# Core target infrastructure
set(UPSTREAM_TARGET_CORE
    ${BM_TARGET}/adi.c
    ${BM_TARGET}/adiv5.c
    ${BM_TARGET}/adiv5_jtag.c
    ${BM_TARGET}/adiv5_swd.c
    ${BM_TARGET}/adiv6.c
    ${BM_TARGET}/cortex.c
    ${BM_TARGET}/cortexm.c
    ${BM_TARGET}/gdb_reg.c
    ${BM_TARGET}/jtag_devs.c
    ${BM_TARGET}/jtag_scan.c
    ${BM_TARGET}/semihosting.c
    ${BM_TARGET}/target.c
    ${BM_TARGET}/target_flash.c
    ${BM_TARGET}/target_probe.c
    ${BM_TARGET}/sfdp.c
    ${BM_TARGET}/spi.c
)

# RISC-V support (needed for ESP32-C3)
set(UPSTREAM_TARGET_RISCV
    ${BM_TARGET}/riscv32.c
    ${BM_TARGET}/riscv64.c
    ${BM_TARGET}/riscv_debug.c
    ${BM_TARGET}/riscv_jtag_dtm.c
)

# Chip-specific targets
set(UPSTREAM_TARGET_CHIPS
    # STM32 family
    ${BM_TARGET}/stm32f1.c
    ${BM_TARGET}/stm32f4.c
    ${BM_TARGET}/stm32g0.c
    ${BM_TARGET}/stm32h5.c
    ${BM_TARGET}/stm32h7.c
    ${BM_TARGET}/stm32l0.c
    ${BM_TARGET}/stm32l4.c
    ${BM_TARGET}/stm32_common.c
    # Nordic
    ${BM_TARGET}/nrf51.c
    ${BM_TARGET}/nrf91.c
    # NXP LPC
    ${BM_TARGET}/lpc11xx.c
    ${BM_TARGET}/lpc15xx.c
    ${BM_TARGET}/lpc17xx.c
    ${BM_TARGET}/lpc40xx.c
    ${BM_TARGET}/lpc43xx.c
    ${BM_TARGET}/lpc546xx.c
    ${BM_TARGET}/lpc55xx.c
    ${BM_TARGET}/lpc_common.c
    # Atmel/Microchip SAM
    ${BM_TARGET}/sam3x.c
    ${BM_TARGET}/sam4l.c
    ${BM_TARGET}/samd.c
    ${BM_TARGET}/samx5x.c
    # Other chips
    ${BM_TARGET}/ch32f1.c
    ${BM_TARGET}/efm32.c
    ${BM_TARGET}/kinetis.c
    ${BM_TARGET}/lmi.c
    ${BM_TARGET}/msp432e4.c
    ${BM_TARGET}/msp432p4.c
    ${BM_TARGET}/nxpke04.c
    ${BM_TARGET}/imxrt.c
    ${BM_TARGET}/hc32l110.c
    ${BM_TARGET}/renesas_ra.c
    ${BM_TARGET}/renesas_rz.c
    # Raspberry Pi
    ${BM_TARGET}/rp2040.c
    ${BM_TARGET}/rp2350.c
)

# Combine all upstream target sources
set(UPSTREAM_TARGET_SOURCES
    ${UPSTREAM_TARGET_CORE}
    ${UPSTREAM_TARGET_RISCV}
    ${UPSTREAM_TARGET_CHIPS}
)

# =============================================================================
# Register component with ESP-IDF
# =============================================================================
idf_component_register(
    SRCS
        ${ESP32_PLATFORM_SOURCES}
        ${UPSTREAM_PLATFORM_SOURCES}
        ${ESP32_CORE_SOURCES}
        ${UPSTREAM_CORE_UTILS}
        ${UPSTREAM_CORE_SOURCES}
        ${CUSTOM_TARGET_SOURCES}
        ${UPSTREAM_TARGET_SOURCES}
    INCLUDE_DIRS
        "${BM_ROOT}/include"
        "${BM_ROOT}/target"
        "${BM_ROOT}/platforms/common"
        "${BM_ROOT}"
        "."
        "target"
        "${CMAKE_SOURCE_DIR}/libopencm3/include"
)

# Add compile definitions
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_BMDA=0
    ENABLE_RTT=1
)
