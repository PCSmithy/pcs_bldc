# Include-path-only seam: exposes the project IO_AS5048_channels.h (the
# channel enum) to the shared io_AS5048 driver. Mirrors pcs_SPI_channels.
# It must carry NO link deps — io_AS5048 links it for the header, and this
# would otherwise close a cycle (io_AS5048_channels -> io_AS5048 -> here).
add_library(pcs_AS5048_channels INTERFACE)
target_include_directories(pcs_AS5048_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver
# (IO_AS5048.h) and HW_SPI (HW_SPI_channel_E).
add_library(io_AS5048_channels STATIC IO_AS5048_channels.c)
target_include_directories(io_AS5048_channels PUBLIC .)
target_link_libraries(io_AS5048_channels PUBLIC io_AS5048 hw_SPI lib_utils)
