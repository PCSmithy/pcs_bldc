# App-layer RGB LED ring UI: display modes driven by the dial + motor encoders,
# rendered to an SK6805 ring, cycled via app_rgbLedRing_cycleMode (the button is
# owned by app_userControls). Channelized (one instance per ring); board wiring
# comes from the consumer-provided app_rgbLedRing_channels seam. Target-
# independent, but only the embedded executable links it (the SIM main() has no
# UI consumer). FreeRTOS-free — run10ms is driven by the shared 10 ms task. The
# unit test compiles app_rgbLedRing.c directly to exercise the render core.
add_library(app_rgbLedRing STATIC
  app_rgbLedRing.c
)
target_include_directories(app_rgbLedRing PUBLIC .)
target_link_libraries(app_rgbLedRing
  PUBLIC
    lib_types
    io_SK6805                 # ring framebuffer + transmit
    io_AS5048                 # dial + motor encoder angles
    app_motorControl          # motor state + setpoint snapshot
    pcs_rgbLedRing_channels   # consumer-provided app_rgbLedRing_channels.h path
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
