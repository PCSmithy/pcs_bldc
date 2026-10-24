# Unit tests for app_rotorEstimation: the module is compiled directly over
# the shared IO_AS5048 boundary double, observed through its public state
# getter. This dir is first on the include path so its channel seam wins.
add_executable(test_app_rotorEstimation
  test_app_rotorEstimation.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../app_rotorEstimation.cpp
)
target_include_directories(test_app_rotorEstimation PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}       # channel seam (first)
  ${CMAKE_CURRENT_SOURCE_DIR}/..    # app_rotorEstimation.h
)
target_link_libraries(test_app_rotorEstimation PRIVATE
  lib_types
  lib_utils
  lib_transforms
  lib_matrix
  unity
  mock_io_AS5048   # shared: shadow IO_AS5048.h + settable angle/status
  m
)
add_test(NAME app_rotorEstimation COMMAND test_app_rotorEstimation)
