# Unit tests for the debounced switch driver. Compiles DEV_switch.c directly
# against test-local channel seams (DEV_switch_channels.h) and a mock HW_GPIO,
# so the debounce logic is exercised in isolation. Native target only.
add_executable(test_dev_switch
  test_DEV_switch.c
  mock_HW_GPIO.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../DEV_switch.c
)
# Test dir first so its mock HW_GPIO.h + channel seam win over the real headers.
target_include_directories(test_dev_switch PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_dev_switch PRIVATE lib_types unity)
add_test(NAME dev_switch COMMAND test_dev_switch)
