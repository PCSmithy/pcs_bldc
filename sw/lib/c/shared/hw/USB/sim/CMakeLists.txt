# Sim HW_USB: a loopback (no TinyUSB). Backs IO_serial's native unit tests and
# carries its own suite covering the CDC byte interface.
add_library(hw_USB STATIC HW_USB.c)
target_include_directories(hw_USB PUBLIC .)
# freertos: HW_USB_run blocks via vTaskDelay so task_usb yields on the
# cooperative fiber scheduler (interim polling seam; see HW_USB.c).
target_link_libraries(hw_USB PUBLIC lib_types freertos)

add_subdirectory(test)
