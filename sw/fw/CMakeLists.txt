# Firmware project — pcs_bldc-driver-board-specific application code that
# integrates the channelized libraries from sw/lib/c/.
#
# Build via:
#   tools/build_native.sh sw/fw    # native (SIL) build
#   tools/build_arm.sh    sw/fw    # cross-compile for STM32G431

cmake_minimum_required(VERSION 3.20)
project(pcs_bldc_fw LANGUAGES C ASM)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

if(PCS_TARGET STREQUAL "native")
  enable_testing()
endif()

# Signal to the lib/c tree that a firmware project consumer is present.
# Channelized hw/ modules and the vendor STM32G4 HAL need consumer-provided
# config interface libs (pcs_ADC_channels, pcs_stm32g4_hal_conf,
# etc.) which only exist inside sw/fw. lib/c gates those modules on this
# variable so a standalone `cmake -S sw/lib/c` build can still build the
# cross-cutting libs (ringbuf, types, build, Unity) and run their tests.
set(PCS_BUILDING_FW TRUE)

# Pull in the shared library tree. lib/c is built in-place from sw/fw's
# build dir; targets like lib_ringbuf, hw_stm32g4, hw_systemClock, etc.
# become available for linking.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/c lib_c_build)

# Application source.
add_subdirectory(src)
