# Unit tests for the board hooks: app_server_config.c compiled against the
# shared app_motorControl and HW_ADC mocks and the REAL sequencer with the
# board's table; the hooks are exercised directly through the config's
# function pointers, a Command through the nanopb decode its arm hook arms.
set(PCS_SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../lib/c/shared)
add_executable(test_app_server_config
  test_app_server_config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_server_config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../sequencer/app_sequencer_channels.c
  ${PCS_SHARED_DIR}/app/sequencer/app_sequencer.cpp
)
target_include_directories(test_app_server_config PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../sequencer   # the board's channel enum + capacity
  ${PCS_SHARED_DIR}/app/sequencer               # app_sequencer.h
)
target_link_libraries(test_app_server_config PRIVATE
  lib_types
  app_server           # app_server.h + transitive frame/serial/binding paths
  mock_app_motorControl   # shared shadow app_motorControl.h + setter recorders
  mock_hw_ADC          # shared shadow HW_ADC.h + injectable volts
  lib_build
  lib_utils
  freertos_host_hooks
  unity
)
add_test(NAME app_server_config COMMAND test_app_server_config)
