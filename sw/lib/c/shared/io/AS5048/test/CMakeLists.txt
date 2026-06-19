# Unit tests for the AS5048 driver. Compiles IO_AS5048.c directly against
# test-local channel seams (IO_AS5048_channels.h, HW_SPI_channels.h) and a
# mock HW_SPI, so the driver is exercised in isolation. Native target only.
add_executable(test_io_AS5048
  test_IO_AS5048.c
  mock_HW_SPI.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_AS5048.c
)
# Test dir first so its mock HW_SPI.h + channel seams win over any real header.
target_include_directories(test_io_AS5048 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_io_AS5048 PRIVATE lib_types unity)
add_test(NAME io_AS5048 COMMAND test_io_AS5048)
