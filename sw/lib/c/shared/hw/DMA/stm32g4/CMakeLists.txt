add_library(hw_DMA STATIC HW_DMA.c)
target_include_directories(hw_DMA PUBLIC .)
target_link_libraries(hw_DMA
  PUBLIC
    lib_types
    stm32g4_hal             # DMA_HandleTypeDef + HAL_DMA_* prototypes
    pcs_DMA_channels        # consumer-provided HW_DMA_channels.h path
)
