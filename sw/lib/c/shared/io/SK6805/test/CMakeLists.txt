# Unit tests for the SK6805 driver. Compiles IO_SK6805.c directly against
# test-local channel seams (IO_SK6805_channels.h, HW_SPI_channels.h) and a
# mock HW_SPI that captures the transmitted buffer. Native target only.
add_executable(test_io_SK6805
  test_IO_SK6805.c
  mock_HW_SPI.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_SK6805.c
)
# Test dir first so its mock HW_SPI.h + channel seams win over any real header.
target_include_directories(test_io_SK6805 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_io_SK6805 PRIVATE lib_types unity)
add_test(NAME io_SK6805 COMMAND test_io_SK6805)
