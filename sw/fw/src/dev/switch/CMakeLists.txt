# Include-path-only seam: exposes dev_switch_channels.h (the channel enum) to
# the shared dev_switch driver. Mirrors pcs_AS5048_channels — no link deps, so
# it can't close a dependency cycle.
add_library(pcs_switch_channels INTERFACE)
target_include_directories(pcs_switch_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver and
# HW_GPIO (port/level enums).
add_library(dev_switch_channels STATIC dev_switch_channels.c)
target_include_directories(dev_switch_channels PUBLIC .)
target_link_libraries(dev_switch_channels PUBLIC dev_switch hw_GPIO lib_utils)
