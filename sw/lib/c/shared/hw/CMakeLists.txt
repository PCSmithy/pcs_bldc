# Hardware abstraction layer implementations.
#
# Only the HAL impl matching the active target is built; the toolchain
# file selects via PCS_TARGET. The vendor STM32G4 HAL only makes sense
# for the embedded target.
#
# When sw/lib/c/hw/sim/ exists with shared simulation utilities (motor
# model, gate-driver model, etc.), add a matching elseif here. For the
# vanilla-compile pass, the native sim board lives entirely under
# sw/fw/src/hw/sim/.

if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
endif()

# HW-layer channelized library modules. Each module has target-specific
# implementations under <module>/<target>/ and decides internally which
# one to add based on PCS_TARGET.
add_subdirectory(systemClock)
add_subdirectory(ADC)
add_subdirectory(OPAMP)
add_subdirectory(GPIO)
add_subdirectory(SPI)
add_subdirectory(I2C)
add_subdirectory(TIM)
add_subdirectory(USB)
add_subdirectory(DMA)
