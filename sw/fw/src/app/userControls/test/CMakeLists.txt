# Unit tests for app_userControls: the module over the shared app_motorControl
# and IO_AS5048 doubles, test-local button and ring seams, and the real
# lib_timer on a fake time source. This dir is first so its seams win.
add_executable(test_app_userControls
  test_app_userControls.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_userControls.c
)
target_include_directories(test_app_userControls PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}       # dev_switch.h, app_rgbLedRing.h seams (first)
  ${CMAKE_CURRENT_SOURCE_DIR}/..    # app_userControls.h
)
# The board's dial full scale, which the real motor-control channel header carries.
target_compile_definitions(test_app_userControls PRIVATE APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC=18.0f)
target_link_libraries(test_app_userControls PRIVATE
  lib_types
  lib_utils
  lib_timer
  unity
  mock_app_motorControl
  mock_io_AS5048
)
add_test(NAME app_userControls COMMAND test_app_userControls)
