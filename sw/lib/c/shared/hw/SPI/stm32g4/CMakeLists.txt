add_library(hw_SPI STATIC HW_SPI.c)
target_include_directories(hw_SPI PUBLIC .)
target_link_libraries(hw_SPI
  PUBLIC
    lib_types
    stm32g4_hal             # for SPI_HandleTypeDef + HAL_SPI_* prototypes
    pcs_SPI_channels        # consumer-provided HW_SPI_channels.h path
    hw_GPIO
    hw_SPI_common           # target-independent timeout formula
)
