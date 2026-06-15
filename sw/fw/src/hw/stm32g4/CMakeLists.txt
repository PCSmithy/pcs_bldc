# Embedded fw_hw library — pcs_bldc-board-specific HAL infrastructure on
# the STM32G431. Just the CubeMX-generated glue (HAL MSP callbacks, IRQ
# handlers, startup file, hal_conf.h, main.h) plus the linker script.
# Application orchestration lives in sw/fw/src/main.c, not here.
#
# Per-module project channel configs (HW_systemClock_config.c, etc.)
# attach to fw_hw via target_sources() from sibling CMakeLists.txt
# files under sw/fw/src/hw/<module>/.
#
# Generated files are managed by tools/convert_cubemx_to_canonical.sh.

# Interface library exposing the HAL conf header location to the vendor
# HAL driver in sw/lib/c/STM32G4xx_HAL_Driver/. The HAL driver has a
# target_link_libraries(... PUBLIC pcs_stm32g4_hal_conf) line that
# forces this to exist before it can compile.
add_library(pcs_stm32g4_hal_conf INTERFACE)
target_include_directories(pcs_stm32g4_hal_conf INTERFACE .)

# FreeRTOSConfig.h lives here too; expose it to the vendored freertos kernel.
add_library(pcs_freertos_config INTERFACE)
target_include_directories(pcs_freertos_config INTERFACE .)

add_library(fw_hw STATIC
  stm32g4xx_hal_msp.c
  stm32g4xx_it.c
  stm32g4xx_hal_timebase_tim.c   # HAL tick on TIM6 (FreeRTOS owns SysTick)
  startup_stm32g431vbtx.s
)

target_include_directories(fw_hw
  PRIVATE
    .   # for "main.h" (peripheral handles, used by hal_msp.c / it.c)
)

target_link_libraries(fw_hw PUBLIC hw_stm32g4 freertos)

# Linker setup propagated to the executable target.
#
# We use --specs=nano.specs (newlib-nano, smaller libc) but NOT
# --specs=nosys.specs — instead we let our syscalls.c provide the
# _read/_write/_close/_lseek/_sbrk/etc. stubs. This keeps the link log
# clean (nosys.specs emits a warning per stub it provides).
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32G431VBTX_FLASH.ld")
target_link_options(fw_hw INTERFACE
  -T${LINKER_SCRIPT}
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/pcs_bldc_fw.map
  --specs=nano.specs
  -Wl,--print-memory-usage
)
