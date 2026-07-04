# Unit tests for the I2C device driver. Compiles IO_i2c.c directly against
# test-local seams (IO_i2c_channels.h, HW_I2C_channels.h, HW_I2C.h) and a mock
# HW_I2C, so the driver is exercised in isolation. Native target only.
add_executable(test_io_i2c
  test_IO_i2c.c
  mock_HW_I2C.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_i2c.c
)
# Test dir first so its mock HW_I2C.h + channel seams win over any real header.
target_include_directories(test_io_i2c PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_io_i2c PRIVATE lib_types unity)
add_test(NAME io_i2c COMMAND test_io_i2c)
