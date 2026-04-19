add_executable(test_lib_ringbuf test_ringbuf.c)
target_link_libraries(test_lib_ringbuf PRIVATE lib_ringbuf unity)
add_test(NAME lib_ringbuf COMMAND test_lib_ringbuf)
