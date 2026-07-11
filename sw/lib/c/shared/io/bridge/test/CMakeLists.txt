# Unit tests for the bridge driver. Compiles IO_bridge.c directly against the
# test-local IO_bridge_channels.h seam and the shared HW_TIM / HW_ADC mocks
# (sw/lib/c/shared/test/mocks/), so the driver is exercised in isolation.
# Native target only.
add_executable(test_io_bridge
  test_IO_bridge.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_bridge.c
)
# Test dir first so its IO_bridge_channels.h seam wins; the shared mock targets
# bring their own shadow HW_TIM.h / HW_ADC.h via their PUBLIC include dirs.
target_include_directories(test_io_bridge PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(test_io_bridge PRIVATE
  lib_types
  unity
  mock_hw_TIM
  mock_hw_ADC
)
add_test(NAME io_bridge COMMAND test_io_bridge)
