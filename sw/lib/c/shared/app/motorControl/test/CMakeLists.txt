# Unit tests for app_motorControl (overcurrent trip + enable gating). Hybrid
# harness: the module-under-test is compiled directly and linked against the
# REAL lib_timer and REAL IO_bridge (over the shared HW_TIM/HW_ADC mocks), the
# shared IO_AS5048 double (fault status), and boundary doubles for
# app_rotorEstimation (rotor angle + velocity) and dev_gateDriver from this dir.
#
# Include order puts this test dir first so its channel seams + boundary-double
# headers (app_rotorEstimation.h, dev_gateDriver.h, *_channels.h) win; the real
# IO_bridge and lib_timer dirs follow for their genuine headers. The shared mocks
# bring their own shadow HW_TIM.h / HW_ADC.h / IO_AS5048.h via the linked targets.
add_executable(test_app_motorControl
  test_app_motorControl.c
  mock_app_rotorEstimation.c
  mock_dev_gateDriver.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_motorControl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../io/bridge/IO_bridge.cpp
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
  lib_pi
  lib_transforms     # SVM — IO_bridge.cpp includes it
  lib_filterIIR      # the bridge's zero-current offset EMA
  hw_CORDIC         # real sim driver (libm) — the step's sine/cosine pair
  unity
  mock_hw_TIM        # shared: shadow HW_TIM.h + recording double
  mock_hw_ADC        # shared: shadow HW_ADC.h + injectable volts
  mock_io_AS5048     # shared: shadow IO_AS5048.h + settable angle/status
  m                 # libm — this harness compiles app_motorControl.cpp itself
)
add_test(NAME app_motorControl COMMAND test_app_motorControl)
