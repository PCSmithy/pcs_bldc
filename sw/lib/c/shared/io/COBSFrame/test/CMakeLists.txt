# Unit tests for IO_COBSFrame. Compiles IO_COBSFrame.c and IO_serial.c directly against
# the test-local channel seams and runs the stack over the real HW_USB sim
# loopback (TX captured / RX injected via the HW_USB_sim hooks). Native only.
add_executable(test_io_COBSFrame
  test_IO_COBSFrame.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../IO_COBSFrame.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../serial/IO_serial.c
)
# Test dir first so its channel seams win; .. and ../../serial for the headers.
target_include_directories(test_io_COBSFrame PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../serial
)
# freertos_host_hooks: hw_USB pulls the kernel (HW_USB_run's vTaskDelay); the
# test never starts the scheduler, so it needs the vApplication* hook stubs.
target_link_libraries(test_io_COBSFrame PRIVATE
  lib_types lib_cobs lib_crc32 hw_USB unity freertos_host_hooks
)
add_test(NAME io_COBSFrame COMMAND test_io_COBSFrame)
