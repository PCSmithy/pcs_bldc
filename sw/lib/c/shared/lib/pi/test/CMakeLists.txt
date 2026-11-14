add_executable(test_lib_pi test_pi.cpp)
target_link_libraries(test_lib_pi PRIVATE lib_pi unity)
add_test(NAME lib_pi COMMAND test_lib_pi)
