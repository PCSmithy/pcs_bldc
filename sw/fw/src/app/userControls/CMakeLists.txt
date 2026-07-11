add_library(app_userControls STATIC
  app_userControls.c
  app_userControls_config.c
)
target_include_directories(app_userControls PUBLIC .)
target_link_libraries(app_userControls
  PUBLIC
    lib_types
    io_AS5048
    dev_switch
    app_motorControl
    app_rgbLedRing
)

if(PCS_TARGET STREQUAL "native")
  # TODO: un-comment when the app_userControls test suite lands — the test/
  # directory is empty and would break the native configure.
  # add_subdirectory(test)
endif()
