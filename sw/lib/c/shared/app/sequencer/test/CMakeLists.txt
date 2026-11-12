# Unit tests for app_sequencer: the module compiled directly over the shared
# app_motorControl double, which records every setter call and serves the
# snapshot a test sets. This dir is first on the include path so its channel
# seam wins.
add_executable(test_app_sequencer
  test_app_sequencer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_sequencer.cpp
)
target_include_directories(test_app_sequencer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}       # channel seam (first)
  ${CMAKE_CURRENT_SOURCE_DIR}/..    # app_sequencer.h
)
target_link_libraries(test_app_sequencer PRIVATE
  lib_types
  lib_utils
  unity
  mock_app_motorControl   # shared: shadow app_motorControl.h + setter recorders
)
add_test(NAME app_sequencer COMMAND test_app_sequencer)
