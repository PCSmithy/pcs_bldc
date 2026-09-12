add_executable(test_lib_utils test_lib_utils.c)
target_link_libraries(test_lib_utils PRIVATE lib_utils lib_types unity)
add_test(NAME lib_utils COMMAND test_lib_utils)
