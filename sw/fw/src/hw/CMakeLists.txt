# pcs_bldc-board-specific hardware abstraction layer.
#
# Both subdirs define a `fw_hw` STATIC library; only the one matching
# PCS_TARGET is added to the build. The executable links against fw_hw
# regardless of target — the toolchain file picks which implementation.

if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
elseif(PCS_TARGET STREQUAL "native")
  add_subdirectory(sim)
else()
  message(FATAL_ERROR
    "Unknown PCS_TARGET '${PCS_TARGET}'; expected 'embedded' or 'native'")
endif()

# Per-module project channel configs that contribute sources + deps to
# the fw_hw library defined above. Added after fw_hw exists so
# target_sources() has something to attach to.
add_subdirectory(systemClock)
add_subdirectory(ADC)
add_subdirectory(GPIO)
add_subdirectory(SPI)
