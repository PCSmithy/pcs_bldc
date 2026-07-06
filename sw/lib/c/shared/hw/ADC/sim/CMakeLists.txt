add_library(hw_ADC STATIC HW_ADC.c)
target_include_directories(hw_ADC PUBLIC .)
target_link_libraries(hw_ADC
  PUBLIC
    lib_types
    pcs_ADC_channels   # consumer-provided HW_ADC_channels.h path
    hw_SIL_ports       # SIL input ports (null-safe when no hooks installed)
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
