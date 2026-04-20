add_library(hw_ADC STATIC HW_ADC.c)
target_include_directories(hw_ADC PUBLIC .)
target_link_libraries(hw_ADC
  PUBLIC
    lib_types
    stm32g4_hal        # for ADC_HandleTypeDef + HAL_ADC_* prototypes
    pcs_ADC_channels   # consumer-provided HW_ADC_channels.h path
)
