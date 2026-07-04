add_executable(test_hw_OPAMP test_HW_OPAMP.c)
target_link_libraries(test_hw_OPAMP PRIVATE hw_OPAMP unity)
add_test(NAME hw_OPAMP COMMAND test_hw_OPAMP)
