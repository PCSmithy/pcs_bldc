add_executable(test_hw_USB test_HW_USB.c)
# freertos_host_hooks: HW_USB_run's vTaskDelay pulls the kernel; the test never
# starts the scheduler, so it needs the vApplication* hook stubs.
target_link_libraries(test_hw_USB PRIVATE hw_USB unity freertos_host_hooks)
add_test(NAME hw_USB COMMAND test_hw_USB)
