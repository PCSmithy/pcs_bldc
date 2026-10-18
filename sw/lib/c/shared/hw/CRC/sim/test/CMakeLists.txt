add_executable(test_hw_CRC test_HW_CRC.c)
target_link_libraries(test_hw_CRC PRIVATE hw_CRC unity)
add_test(NAME hw_CRC COMMAND test_hw_CRC)
