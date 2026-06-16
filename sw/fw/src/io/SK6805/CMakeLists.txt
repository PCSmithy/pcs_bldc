# Include-path-only seam: exposes the project IO_SK6805_config.h
# (IO_SK6805_PIXEL_COUNT) to the shared io_SK6805 driver. Mirrors
# pcs_AS5048_channels — no link deps, so it can't close a dependency cycle.
add_library(pcs_SK6805_config INTERFACE)
target_include_directories(pcs_SK6805_config INTERFACE .)

# Project config translation unit. Pulls in the shared driver (IO_SK6805.h)
# and HW_SPI (HW_SPI_channel_E).
add_library(io_SK6805_config STATIC IO_SK6805_config.c)
target_include_directories(io_SK6805_config PUBLIC .)
target_link_libraries(io_SK6805_config PUBLIC io_SK6805 hw_SPI)
