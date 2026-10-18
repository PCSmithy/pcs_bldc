# Channelized serial transport (target-independent — talks to its backing
# transport only through the HW_USB CDC API). Dual-target: native links the
# HW_USB sim loopback and carries a Unity suite.
add_library(io_serial STATIC IO_serial.c)
target_include_directories(io_serial PUBLIC .)
target_link_libraries(io_serial
  PUBLIC
    lib_types
    hw_USB                  # HW_USB.h: the CDC byte interface
    pcs_serial_channels     # consumer-provided IO_serial_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
