
add_library(app_motorControl STATIC
  app_motorControl.c
)
target_include_directories(app_motorControl PUBLIC .)
target_link_libraries(app_motorControl
  PUBLIC
    lib_types
    lib_timer
    lib_filterIIR
    io_bridge
    io_AS5048
    dev_gateDriver
    pcs_motorControl_channels
  PRIVATE
    m   # fmodf — glibc keeps libm separate; harmless on MinGW/macOS/newlib
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
