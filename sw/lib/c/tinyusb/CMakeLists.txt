# TinyUSB (vendor, third-party, MIT) — USB device stack.
#
# Vendored subset: core + device + CDC class + the STM32 FS device controller
# (portable/st/stm32_fsdev) + native FreeRTOS osal. Host, TypeC, and the other
# device classes are intentionally not vendored. Embedded only.
#
# Needs a consumer-provided `pcs_tinyusb_config` interface library that points
# at the project's tusb_config.h. Sources are a vendored snapshot (TinyUSB
# 0.20.0); refresh by re-cloning the tag, not via the CubeMX convert script.

add_library(tinyusb STATIC
  tusb.c
  common/tusb_fifo.c
  device/usbd.c
  device/usbd_control.c
  class/cdc/cdc_device.c
  portable/st/stm32_fsdev/dcd_stm32_fsdev.c
)

# TinyUSB sources include each other relative to this root (e.g.
# "device/usbd.h", "common/tusb_common.h").
target_include_directories(tinyusb PUBLIC .)

# The fsdev controller reaches the USB peripheral registers through the ST
# CMSIS device header, which requires the device-select define.
target_compile_definitions(tinyusb PUBLIC STM32G431xx)

# Vendor code: suppress our project-wide -Wall/-Wextra/-Wpedantic.
target_compile_options(tinyusb PRIVATE -w)

# TinyUSB relies on GNU C extensions (asm(), statement expressions, ...). Our
# project default is strict -std=c11 (C_EXTENSIONS OFF); compile this vendor
# lib as gnu11. Scope is local to the target.
set_target_properties(tinyusb PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

# cmsis: STM32G4 device + Cortex-M core headers. freertos: osal_freertos.h
# pulls in FreeRTOS.h/task.h/semphr.h/queue.h. pcs_tinyusb_config: tusb_config.h.
target_link_libraries(tinyusb PUBLIC cmsis freertos pcs_tinyusb_config)
