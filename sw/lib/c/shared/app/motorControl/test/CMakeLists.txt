# Unit tests for app_motorControl (overcurrent trip + enable gating). Hybrid
# harness: the module-under-test is compiled directly and linked against the
# REAL lib_timer and REAL IO_bridge (over the shared HW_TIM/HW_ADC mocks), with
# boundary doubles for IO_AS5048 + dev_gateDriver supplied from this dir.
#
# Include order puts this test dir first so its channel seams + boundary-double
# headers (IO_AS5048.h, dev_gateDriver.h, *_channels.h) win; the real IO_bridge
# and lib_timer dirs follow for their genuine headers. The shared HW mocks bring
# their own shadow HW_TIM.h / HW_ADC.h via the linked mock targets.
add_executable(test_app_motorControl
  test_app_motorControl.c
  mock_IO_AS5048.c
  mock_dev_gateDriver.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_motorControl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../io/bridge/IO_bridge.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/timer/lib_timer.c
)
target_include_directories(test_app_motorControl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}                        # seams + boundary doubles (first)
  ${CMAKE_CURRENT_SOURCE_DIR}/..                     # app_motorControl.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../io/bridge     # real IO_bridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/timer     # real lib_timer.h
)
target_link_libraries(test_app_motorControl PRIVATE
  lib_types
  lib_utils
  unity
  mock_hw_TIM        # shared: shadow HW_TIM.h + recording double
  mock_hw_ADC        # shared: shadow HW_ADC.h + injectable volts
  m                  # fmodf — this harness compiles app_motorControl.c itself
)
add_test(NAME app_motorControl COMMAND test_app_motorControl)
