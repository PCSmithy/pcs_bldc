add_executable(test_hw_USB test_HW_USB.c)
target_link_libraries(test_hw_USB PRIVATE hw_USB unity)
add_test(NAME hw_USB COMMAND test_hw_USB)
