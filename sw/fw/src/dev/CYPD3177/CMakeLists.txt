# Include-path-only seam: exposes dev_CYPD3177_channels.h (the channel enum) to
# the shared dev_CYPD3177 driver. Mirrors pcs_switch_channels — no link deps, so
# it can't close a dependency cycle.
add_library(pcs_CYPD3177_channels INTERFACE)
target_include_directories(pcs_CYPD3177_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver (which
# brings io_i2c for the IO_i2c device enum) and lib_utils (COUNTOF).
add_library(dev_CYPD3177_channels STATIC dev_CYPD3177_channels.c)
target_include_directories(dev_CYPD3177_channels PUBLIC .)
target_link_libraries(dev_CYPD3177_channels PUBLIC dev_CYPD3177 lib_utils)
