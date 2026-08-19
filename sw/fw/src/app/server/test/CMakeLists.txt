# Unit tests for the board hooks: app_server_config.c compiled against the
# test-local app_motorControl shadow mock and the shared HW_ADC mock; the
# hooks are exercised directly through the config's function pointers.
add_executable(test_app_server_config
  test_app_server_config.c
  mock_app_motorControl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_server_config.c
)
# Test dir first so the shadow app_motorControl.h wins.
target_include_directories(test_app_server_config PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_app_server_config PRIVATE
  app_server           # app_server.h + transitive frame/serial/binding paths
  mock_hw_ADC          # shared shadow HW_ADC.h + injectable volts
  lib_build
  lib_utils
  freertos_host_hooks
  unity
)
add_test(NAME app_server_config COMMAND test_app_server_config)
