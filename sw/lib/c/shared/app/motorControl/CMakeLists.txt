
add_library(app_motorControl STATIC
  app_motorControl.cpp
)
target_include_directories(app_motorControl PUBLIC .)
target_link_libraries(app_motorControl
  PUBLIC
    lib_types
    lib_timer
    lib_transforms
    lib_pi
    hw_CORDIC
    app_rotorEstimation
    io_bridge
    io_AS5048
    dev_gateDriver
    pcs_motorControl_channels
  PRIVATE
    m   # libm — glibc keeps it separate; harmless on MinGW/macOS/newlib
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
