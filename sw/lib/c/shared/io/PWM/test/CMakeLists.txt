# Unit tests for the PWM driver. Compiles IO_PWM.c directly against test-local
# seams (IO_PWM_channels.h, HW_TIM_channels.h, HW_TIM.h) and a mock HW_TIM, so
# the driver is exercised in isolation. Native target only.
add_executable(test_io_PWM
  test_IO_PWM.c
  mock_HW_TIM.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_PWM.c
)
# Test dir first so its mock HW_TIM.h + channel seams win over any real header.
target_include_directories(test_io_PWM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_io_PWM PRIVATE lib_types unity)
add_test(NAME io_PWM COMMAND test_io_PWM)
