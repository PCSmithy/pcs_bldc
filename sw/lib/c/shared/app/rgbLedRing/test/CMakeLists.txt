# Unit tests for the RGB-ring render core. Compiles app_rgbLedRing.c directly
# against test-local mock headers (FreeRTOS + IO/dev + the channel seam) and
# no-op stubs in mock_app_deps.c, so the mode FSM, per-mode rendering, and colour
# persistence are exercised without the real kernel or drivers. Native only.
add_executable(test_app_rgbLedRing
  test_app_rgbLedRing.c
  mock_app_deps.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_rgbLedRing.c
)
# Test dir first so the mock FreeRTOS/IO/dev headers + channel seam win over the
# real ones; the parent dir provides app_rgbLedRing.h.
target_include_directories(test_app_rgbLedRing PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_app_rgbLedRing PRIVATE lib_types unity)
add_test(NAME app_rgbLedRing COMMAND test_app_rgbLedRing)
