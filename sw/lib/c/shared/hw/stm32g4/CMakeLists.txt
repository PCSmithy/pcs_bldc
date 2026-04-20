# STM32G4-family HW abstraction layer (firmware project's view of the
# STM32G4 family). Eventually holds our channelized SPI / UART / ADC /
# GPIO / etc. wrappers around the vendor HAL.
#
# For the vanilla-compile pass we only need the vendor support files
# (system_stm32g4xx.c — SystemInit / SystemCoreClockUpdate; syscalls.c
# and sysmem.c — newlib stubs). The actual HAL driver lives at the top
# level: sw/lib/c/STM32G4xx_HAL_Driver/, target `stm32g4_hal`.
#
# system_stm32g4xx.c, syscalls.c, sysmem.c are auto-managed by
# tools/convert_cubemx_to_canonical.sh. Do not hand-edit them.

add_library(hw_stm32g4 STATIC
  system_stm32g4xx.c
  syscalls.c
  sysmem.c
)

# Vendor support files — same warning-suppression as the HAL driver.
target_compile_options(hw_stm32g4 PRIVATE -w)

# Pull in the HAL driver (and transitively cmsis +
# pcs_stm32g4_hal_conf) so that consumers of hw_stm32g4 don't have to
# remember to link them separately.
target_link_libraries(hw_stm32g4 PUBLIC stm32g4_hal)
