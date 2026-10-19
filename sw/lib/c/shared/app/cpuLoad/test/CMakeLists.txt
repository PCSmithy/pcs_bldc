# The module under test with the real lib_timer over a test-supplied time base.
add_executable(test_app_cpuLoad
  test_app_cpuLoad.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_cpuLoad.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/timer/lib_timer.c
)
target_include_directories(test_app_cpuLoad PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/timer
)
target_link_libraries(test_app_cpuLoad PRIVATE lib_types lib_utils unity)
add_test(NAME app_cpuLoad COMMAND test_app_cpuLoad)
