# Sim HW_USB: a loopback (no TinyUSB). Backs IO_serial's native unit tests and
# carries its own suite covering the CDC byte interface.
add_library(hw_USB STATIC HW_USB.c)
target_include_directories(hw_USB PUBLIC .)
target_link_libraries(hw_USB PUBLIC lib_types)

add_subdirectory(test)
