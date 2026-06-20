# Include-path-only seam: exposes the project IO_SK6805_channels.h (channel
# enum + IO_SK6805_PIXEL_COUNT) to the shared io_SK6805 driver. Mirrors
# pcs_AS5048_channels — no link deps, so it can't close a dependency cycle.
add_library(pcs_SK6805_channels INTERFACE)
target_include_directories(pcs_SK6805_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver
# (IO_SK6805.h), HW_SPI (HW_SPI_channel_E), and lib_utils (COUNTOF).
add_library(io_SK6805_channels STATIC IO_SK6805_channels.c)
target_include_directories(io_SK6805_channels PUBLIC .)
target_link_libraries(io_SK6805_channels PUBLIC io_SK6805 hw_SPI lib_utils)
