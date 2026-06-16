# Channelized SK6805 RGB-LED-string driver (target-independent — drives the
# string through the HW_SPI MOSI-bit-expansion channel).
add_library(io_SK6805 STATIC IO_SK6805.c)
target_include_directories(io_SK6805 PUBLIC .)
target_link_libraries(io_SK6805
  PUBLIC
    lib_types               # fixed-width types
    hw_SPI                  # HW_SPI.h: HW_SPI_channel_E + HW_SPI_transmit
    pcs_SK6805_config       # consumer-provided IO_SK6805_config.h path
)
