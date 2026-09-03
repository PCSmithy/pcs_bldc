add_library(hw_TIM STATIC HW_TIM.c)
target_include_directories(hw_TIM PUBLIC . ..)
target_link_libraries(hw_TIM
  PUBLIC
    lib_types
    pcs_TIM_channels   # consumer-provided HW_TIM_channels.h path
    hw_SIL_ports       # PWM/bridge observation ports (null-safe when no hooks installed)
)

# Unit tests run only on the native target.
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
