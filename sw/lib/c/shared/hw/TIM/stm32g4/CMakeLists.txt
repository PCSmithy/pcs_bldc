add_library(hw_TIM STATIC HW_TIM.c)
target_include_directories(hw_TIM PUBLIC .)
target_link_libraries(hw_TIM
  PUBLIC
    lib_types
    stm32g4_hal        # for TIM_HandleTypeDef + HAL_TIM_* prototypes
    pcs_TIM_channels   # consumer-provided HW_TIM_channels.h path
)
