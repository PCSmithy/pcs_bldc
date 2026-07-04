# Unit tests for the CYPD3177 runtime driver. Compiles dev_CYPD3177.c directly
# against test-local seams (IO_i2c.h, IO_i2c_channels.h, dev_CYPD3177_channels.h)
# and a mock IO_i2c, linking the real lib_CYPD3177 decoders. Native target only.
add_executable(test_dev_CYPD3177
  test_dev_CYPD3177.c
  mock_IO_i2c.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../dev_CYPD3177.c
)
# Test dir first so its mock IO_i2c.h + channel seams win over the real headers.
target_include_directories(test_dev_CYPD3177 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_dev_CYPD3177 PRIVATE lib_types lib_CYPD3177 unity)
add_test(NAME dev_CYPD3177 COMMAND test_dev_CYPD3177)
