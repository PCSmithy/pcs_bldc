add_executable(test_lib_timer test_lib_timer.c)
target_link_libraries(test_lib_timer PRIVATE lib_timer unity)
add_test(NAME lib_timer COMMAND test_lib_timer)
