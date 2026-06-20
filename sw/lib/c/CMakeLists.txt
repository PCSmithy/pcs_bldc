# sw/lib/c — channelized library implementations (the "platform").
#
# Top-level layout:
#   CMSIS/                 vendor (ARM Cortex-M standard, header-only)
#   STM32G4xx_HAL_Driver/  vendor (ST HAL driver, embedded only)
#   Unity/                 vendor (test framework, native only)
#   shared/                OUR library code, layered (app/dev/io/hw/lib)
#
# Each library under shared/ is a STATIC archive consumable by either
# the firmware project (sw/fw) or the future Rust app (via FFI). All
# libraries here must build for both PCS_TARGET=native and
# PCS_TARGET=embedded.
#
# Library naming convention: <layer>_<module>
#   layer  ∈ {app, dev, io, hw, lib}
#   module is the module's directory name
#   e.g. sw/lib/c/shared/lib/ringbuf/  ->  target name: lib_ringbuf

cmake_minimum_required(VERSION 3.20)
project(pcs_lib_c LANGUAGES C ASM)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# Tests are only meaningful (and runnable) on the native target. Unity
# (vendored at sw/lib/c/Unity) is only built when tests will be built.
if(PCS_TARGET STREQUAL "native")
  enable_testing()
  add_subdirectory(Unity)
endif()

# Vendored third-party libraries that aren't tied to a specific layer.
add_subdirectory(CMSIS)
if(PCS_TARGET STREQUAL "embedded" AND PCS_BUILDING_FW)
  # The STM32G4 HAL driver is family-specific (ARM intrinsics, doesn't
  # compile natively) AND needs a consumer-provided pcs_stm32g4_hal_conf
  # interface lib for stm32g4xx_hal_conf.h. Both conditions must hold.
  add_subdirectory(STM32G4xx_HAL_Driver)
endif()

if(PCS_BUILDING_FW)
  # FreeRTOS kernel — dual-target: Cortex-M4F port on embedded, the
  # cooperative fiber port on native (SIL). Needs the consumer-provided
  # pcs_freertos_config interface lib for the target-appropriate
  # FreeRTOSConfig.h. (See FreeRTOS/CMakeLists.txt.)
  add_subdirectory(FreeRTOS)
endif()

if(PCS_TARGET STREQUAL "embedded" AND PCS_BUILDING_FW)
  # TinyUSB USB device stack (stm32_fsdev) — embedded only. Needs the
  # consumer-provided pcs_tinyusb_config interface lib for tusb_config.h.
  add_subdirectory(tinyusb)
endif()

# Our own library code (layered).
add_subdirectory(shared)
