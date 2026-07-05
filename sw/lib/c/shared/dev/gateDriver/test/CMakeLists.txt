# Unit tests for the gate-driver runtime driver. Compiles dev_gateDriver.c
# directly against test-local seams (IO_i2c.h, IO_i2c_channels.h,
# dev_gateDriver_channels.h) and a read+write mock IO_i2c. Native target only.
add_executable(test_dev_gateDriver
  test_dev_gateDriver.c
  mock_IO_i2c.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../dev_gateDriver.c
)
# Test dir first so its mock IO_i2c.h + channel seams win over the real headers.
target_include_directories(test_dev_gateDriver PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_dev_gateDriver PRIVATE lib_types unity)
add_test(NAME dev_gateDriver COMMAND test_dev_gateDriver)
