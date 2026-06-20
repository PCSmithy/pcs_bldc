# Unit tests for IO_serial. Compiles IO_serial.c directly against the test-local
# channel seam and runs it over the real HW_USB sim loopback (driven via the
# HW_USB_sim hooks). Native target only.
add_executable(test_io_serial
  test_IO_serial.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_serial.c
)
# Test dir first so its channel seam wins; .. for IO_serial.h.
target_include_directories(test_io_serial PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_io_serial PRIVATE lib_types hw_USB unity)
add_test(NAME io_serial COMMAND test_io_serial)
