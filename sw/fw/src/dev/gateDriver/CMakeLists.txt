# Include-path-only seam: exposes dev_gateDriver_channels.h (the channel enum)
# to the shared dev_gateDriver driver. Mirrors pcs_CYPD3177_channels — no link
# deps, so it can't close a dependency cycle.
add_library(pcs_gateDriver_channels INTERFACE)
target_include_directories(pcs_gateDriver_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver (which
# brings io_i2c for the IO_i2c device enum) and lib_utils (COUNTOF).
add_library(dev_gateDriver_channels STATIC dev_gateDriver_channels.c)
target_include_directories(dev_gateDriver_channels PUBLIC .)
target_link_libraries(dev_gateDriver_channels PUBLIC dev_gateDriver lib_utils)
