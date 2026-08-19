# app_server's project side: the board hooks (telemetry assembly, command
# handling) and channel wiring behind the generic server core.
add_library(app_server_config STATIC app_server_config.c)
target_include_directories(app_server_config PUBLIC .)
target_link_libraries(app_server_config
  PUBLIC
    app_server
    app_motorControl
    hw_ADC
    lib_build
    lib_utils
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
