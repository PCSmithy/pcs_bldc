# FreeRTOS Kernel V10.3.1 (vendor, third-party).
#
# Dual-target:
#   - embedded: the Cortex-M4F port (GCC/ARM_CM4F) + cmsis.
#   - native:   our cooperative single-threaded FIBER port (Native-Fiber),
#               validated by the D1 spike. No cmsis. See
#               docs/sil/freertos-tick.md / performance.md.
#
# The CMSIS-RTOS2 wrapper (cmsis_os2.c) is intentionally NOT vendored —
# application code uses the native FreeRTOS API directly.
#
# Both sides need a consumer-provided `pcs_freertos_config` interface library
# that points at the target-appropriate FreeRTOSConfig.h (board file on
# embedded, sim file on native).
#
# Kernel sources are a vendored snapshot; this CMakeLists.txt is hand-written
# and preserved across regenerations.

if(PCS_TARGET STREQUAL "embedded")
  set(FREERTOS_PORT_SRC  portable/GCC/ARM_CM4F/port.c)
  set(FREERTOS_PORT_INC  portable/GCC/ARM_CM4F)
  set(FREERTOS_PORT_DEPS cmsis)
else()
  set(FREERTOS_PORT_SRC  portable/Native-Fiber/port.c)
  set(FREERTOS_PORT_INC  portable/Native-Fiber)
  set(FREERTOS_PORT_DEPS )
endif()

add_library(freertos STATIC
  tasks.c
  queue.c
  list.c
  timers.c
  event_groups.c
  stream_buffer.c
  croutine.c
  ${FREERTOS_PORT_SRC}
  portable/MemMang/heap_4.c
)

target_include_directories(freertos PUBLIC
  include
  ${FREERTOS_PORT_INC}
)

# Vendor code: suppress our project-wide -Wall/-Wextra/-Wpedantic.
target_compile_options(freertos PRIVATE -w)

# `pcs_freertos_config` (consumer-provided) supplies FreeRTOSConfig.h.
# Embedded additionally needs `cmsis` (Cortex-M core headers + device header).
target_link_libraries(freertos PUBLIC ${FREERTOS_PORT_DEPS} pcs_freertos_config)
