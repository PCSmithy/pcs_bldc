# Channelized frame transport over IO_serial: COBS-delimited, CRC-32-validated
# frames (fw~conn_proto). Target-independent — dual-target through io_serial's
# HW_USB seam; native carries a Unity suite over the HW_USB sim loopback.
add_library(io_COBSFrame STATIC IO_COBSFrame.c)
target_include_directories(io_COBSFrame PUBLIC .)
target_link_libraries(io_COBSFrame
  PUBLIC
    lib_types
    lib_cobs
    lib_crc32
    io_serial
    pcs_COBSFrame_channels     # consumer-provided IO_COBSFrame_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
