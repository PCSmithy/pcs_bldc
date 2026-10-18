# Generic protocol server: envelope dispatch with correlated replies, the
# framework services (ping, identity, log stream), and the telemetry cadence,
# over the frame driver. Board specifics enter only through the config's
# board.Request/board.Telemetry hooks and the project-generated schema
# bindings (lib_protobuf_config).
add_library(app_server STATIC app_server.c app_server_trace.c)
target_include_directories(app_server PUBLIC .)
target_link_libraries(app_server
  PUBLIC
    lib_types
    lib_ringbuf
    lib_protobuf
    lib_protobuf_config   # project schema-bindings seam
    lib_build_config      # project build-identity seam
    io_COBSFrame
    freertos
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
