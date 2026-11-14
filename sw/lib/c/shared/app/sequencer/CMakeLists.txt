add_library(app_sequencer STATIC
  app_sequencer.cpp
)
target_include_directories(app_sequencer PUBLIC .)
target_link_libraries(app_sequencer
  PUBLIC
    lib_types
    app_motorControl
    pcs_sequencer_channels
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
