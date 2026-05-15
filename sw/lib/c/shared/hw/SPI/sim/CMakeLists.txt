add_library(hw_SPI STATIC HW_SPI.c)
target_include_directories(hw_SPI PUBLIC .)
target_link_libraries(hw_SPI
  PUBLIC
    lib_types
    pcs_SPI_channels        # consumer-provided HW_SPI_channels.h path
)
