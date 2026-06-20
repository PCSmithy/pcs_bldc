add_executable(test_hw_TIM test_HW_TIM.c)
target_link_libraries(test_hw_TIM PRIVATE hw_TIM unity)
add_test(NAME hw_TIM COMMAND test_hw_TIM)
