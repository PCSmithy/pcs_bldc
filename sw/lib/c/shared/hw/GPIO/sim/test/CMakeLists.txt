add_executable(test_hw_GPIO test_HW_GPIO.c)
target_link_libraries(test_hw_GPIO PRIVATE hw_GPIO lib_utils unity)
add_test(NAME hw_GPIO COMMAND test_hw_GPIO)
