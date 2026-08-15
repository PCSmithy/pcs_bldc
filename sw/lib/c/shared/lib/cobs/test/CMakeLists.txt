add_executable(test_lib_cobs test_cobs.c)
target_link_libraries(test_lib_cobs PRIVATE lib_cobs unity)
add_test(NAME lib_cobs COMMAND test_lib_cobs)
