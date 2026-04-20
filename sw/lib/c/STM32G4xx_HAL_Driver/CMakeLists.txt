# ST STM32G4xx HAL Driver (vendor, third-party).
#
# Family-specific peripheral driver layer that the rest of the firmware
# wraps with our own channelized abstractions in sw/lib/c/hw/stm32g4/.
#
# Compiles only when the consumer (sw/fw) provides a
# `pcs_stm32g4_hal_conf` interface library that points at the
# pcs_bldc-board-specific stm32g4xx_hal_conf.h.
#
# Vendor sources (Inc/, Src/, LICENSE.txt) are auto-managed by
# tools/convert_cubemx_to_canonical.sh; this CMakeLists.txt is
# hand-written and the script preserves it.

file(GLOB HAL_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.c"
)
list(FILTER HAL_SOURCES EXCLUDE REGEX "_template\\.c$")

add_library(stm32g4_hal STATIC ${HAL_SOURCES})

target_include_directories(stm32g4_hal PUBLIC Inc)

target_compile_definitions(stm32g4_hal PUBLIC
  USE_HAL_DRIVER
  STM32G431xx
)

# Vendor code: suppress our project-wide -Wall/-Wextra/-Wpedantic so
# upstream warnings (e.g. empty translation units in conditional LL
# sources) don't pollute the build log.
target_compile_options(stm32g4_hal PRIVATE -w)

# `cmsis` provides Cortex-M Core + STM32G4 device headers.
# `pcs_stm32g4_hal_conf` (provided by the consumer) provides the
# stm32g4xx_hal_conf.h that selects which HAL modules are enabled.
target_link_libraries(stm32g4_hal PUBLIC cmsis pcs_stm32g4_hal_conf)
