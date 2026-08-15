add_executable(test_lib_crc32 test_crc32.c)
target_link_libraries(test_lib_crc32 PRIVATE lib_crc32 unity)
add_test(NAME lib_crc32 COMMAND test_lib_crc32)
