# FreeRTOS Kernel V10.3.1 (vendor, third-party).
#
# Native FreeRTOS kernel for the STM32G431 (Cortex-M4F). The CMSIS-RTOS2
# wrapper (cmsis_os2.c) is intentionally NOT vendored — application code uses
# the native FreeRTOS API (xTaskCreate / vTaskDelay / queues) directly.
#
# Compiles only on the embedded target. Needs a consumer-provided
# `pcs_freertos_config` interface library that points at the
# pcs_bldc-board-specific FreeRTOSConfig.h.
#
# Kernel sources are a vendored snapshot managed by
# tools/convert_cubemx_to_canonical.sh; this CMakeLists.txt is hand-written
# and the script preserves it.

add_library(freertos STATIC
  tasks.c
  queue.c
  list.c
  timers.c
  event_groups.c
  stream_buffer.c
  croutine.c
  portable/GCC/ARM_CM4F/port.c
  portable/MemMang/heap_4.c
)

target_include_directories(freertos PUBLIC
  include
  portable/GCC/ARM_CM4F
)

# Vendor code: suppress our project-wide -Wall/-Wextra/-Wpedantic.
target_compile_options(freertos PRIVATE -w)

# `cmsis` provides the Cortex-M core headers (NVIC priorities, intrinsics) +
# the STM32G4 device header referenced by FreeRTOSConfig.h. `pcs_freertos_config`
# (consumer-provided) supplies the project's FreeRTOSConfig.h.
target_link_libraries(freertos PUBLIC cmsis pcs_freertos_config)
