add_library(hw_OPAMP STATIC HW_OPAMP.c)
target_include_directories(hw_OPAMP PUBLIC . ..)
target_link_libraries(hw_OPAMP
  PUBLIC
    lib_types
    stm32g4_hal          # for OPAMP_HandleTypeDef + HAL_OPAMP_* prototypes
    pcs_OPAMP_channels   # consumer-provided HW_OPAMP_channels.h path
)
