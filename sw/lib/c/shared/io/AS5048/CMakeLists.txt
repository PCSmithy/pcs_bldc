# Channelized AS5048 encoder driver (target-independent — talks to the
# encoder only through the HW_SPI channel API).
add_library(io_AS5048 STATIC IO_AS5048.c)
target_include_directories(io_AS5048 PUBLIC .)
target_link_libraries(io_AS5048
  PUBLIC
    lib_types               # float32_t + fixed-width types
    hw_SPI                  # HW_SPI.h: HW_SPI_channel_E + HW_SPI_transmitReceive
    pcs_AS5048_channels     # consumer-provided IO_AS5048_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
