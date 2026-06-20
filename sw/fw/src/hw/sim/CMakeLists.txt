# Native (SIL) fw_hw — STATIC for symmetry with the embedded side, so
# per-module project channel configs (HW_*_config.c) can attach via
# target_sources(fw_hw PRIVATE ...) without needing the INTERFACE
# keyword juggling that an INTERFACE fw_hw would force.
#
# _placeholder.c exists only to give the STATIC library a source file
# (CMake errors on a STATIC library with no sources). When real sim
# infrastructure lands here, delete _placeholder.c.

add_library(fw_hw STATIC _placeholder.c)

# FreeRTOSConfig.h for the native fiber port lives here; expose it to the
# vendored freertos kernel (consumer-provided config, mirrors the embedded
# pcs_freertos_config in hw/stm32g4).
add_library(pcs_freertos_config INTERFACE)
target_include_directories(pcs_freertos_config INTERFACE .)

# Native FreeRTOS (cooperative fiber port) is available to the firmware in SIL.
target_link_libraries(fw_hw PUBLIC freertos)
