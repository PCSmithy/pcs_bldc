add_executable(test_hw_CORDIC test_HW_CORDIC.c)
target_link_libraries(test_hw_CORDIC PRIVATE hw_CORDIC unity)
add_test(NAME hw_CORDIC COMMAND test_hw_CORDIC)
