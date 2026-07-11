# pcs_bridge_channels: include-path-only seam exposing the project
# IO_bridge_channels.h (bridge channel enum) to the shared io_bridge driver.
# Carries NO link deps — io_bridge links it for the header, and a dep here would
# close a cycle (io_bridge_channels -> io_bridge -> here).
add_library(pcs_bridge_channels INTERFACE)
target_include_directories(pcs_bridge_channels INTERFACE .)

# Project bridge-config translation unit. Pulls in the shared driver (IO_bridge.h)
# and HW_TIM (HW_TIM_channels_E).
add_library(io_bridge_channels STATIC IO_bridge_channels.c)
target_include_directories(io_bridge_channels PUBLIC .)
target_link_libraries(io_bridge_channels PUBLIC io_bridge hw_TIM lib_utils)
