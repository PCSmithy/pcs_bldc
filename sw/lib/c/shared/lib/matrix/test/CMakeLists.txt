add_executable(test_lib_matrix test_matrix.cpp)
target_link_libraries(test_lib_matrix PRIVATE lib_matrix unity)
add_test(NAME lib_matrix COMMAND test_lib_matrix)
