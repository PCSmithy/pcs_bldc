# Hardware abstraction layer implementations.
#
# Only the HAL impl matching the active target is built; the toolchain
# file selects via PCS_TARGET. The vendor STM32G4 HAL only makes sense
# for the embedded target; sim/ holds shared native simulation support
# (the SIL port seam now; motor / gate-driver models later).

if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
elseif(PCS_TARGET STREQUAL "native")
  add_subdirectory(sim)
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
