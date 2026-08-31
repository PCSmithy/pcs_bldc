# Include-path-only seam: exposes the project IO_COBSFrame_channels.h (channel
# enum + payload bound) to the shared io_COBSFrame driver. Mirrors
# pcs_serial_channels — no link deps, so it can't close a dependency cycle.
add_library(pcs_COBSFrame_channels INTERFACE)
target_include_directories(pcs_COBSFrame_channels INTERFACE .)

# Project channel-config translation unit. Pulls in the shared driver
# (IO_COBSFrame.h) and lib_utils (COUNTOF).
add_library(io_COBSFrame_channels STATIC IO_COBSFrame_channels.c)
target_include_directories(io_COBSFrame_channels PUBLIC .)
target_link_libraries(io_COBSFrame_channels PUBLIC io_COBSFrame lib_utils)
