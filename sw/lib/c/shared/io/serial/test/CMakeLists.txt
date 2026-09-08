# Unit tests for IO_serial. Compiles IO_serial.c directly against the test-local
# channel seam and a boundary mock of the HW_USB API — no sim driver, no kernel.
add_executable(test_io_serial
  test_IO_serial.c
  mock_HW_USB.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_serial.c
)
# Test dir first so its channel seam wins; .. for IO_serial.h; the HW_USB module
# root for the HW_USB.h API the mock implements.
target_include_directories(test_io_serial PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../hw/USB
)
target_link_libraries(test_io_serial PRIVATE lib_types unity)
add_test(NAME io_serial COMMAND test_io_serial)
