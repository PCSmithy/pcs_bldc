add_executable(test_hw_I2C test_HW_I2C.c)
target_link_libraries(test_hw_I2C PRIVATE hw_I2C unity)
add_test(NAME hw_I2C COMMAND test_hw_I2C)
