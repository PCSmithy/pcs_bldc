# Unit tests for the generic server core: the real frame/serial/USB-sim stack
# underneath, with test-local stub board hooks. Native only.
add_executable(test_app_server test_app_server.c)
target_link_libraries(test_app_server PRIVATE
  app_server
  lib_cobs lib_crc32
  freertos_host_hooks
  unity
)
add_test(NAME app_server COMMAND test_app_server)
