# Include-path-only seam: exposes the project IO_serial_channels.h (channel
# enum) to the shared io_serial driver. Mirrors pcs_AS5048_channels — no link
# deps, so it can't close a dependency cycle.
add_library(pcs_serial_channels INTERFACE)
target_include_directories(pcs_serial_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver
# (IO_serial.h) and lib_utils (COUNTOF).
add_library(io_serial_channels STATIC IO_serial_channels.c)
target_include_directories(io_serial_channels PUBLIC .)
target_link_libraries(io_serial_channels PUBLIC io_serial lib_utils)
