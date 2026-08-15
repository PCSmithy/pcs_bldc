# Sim HW_USB: a loopback (no TinyUSB). Backs IO_serial's native unit tests and
# carries its own suite covering the CDC byte interface.
add_library(hw_USB STATIC HW_USB.c)
target_include_directories(hw_USB PUBLIC .)
# freertos: HW_USB_run blocks task_usb until the simulated USB interrupt wakes
# it. hw_SIL_irq is the seam that interrupt is registered through (see HW_USB.c).
target_link_libraries(hw_USB PUBLIC lib_types freertos hw_SIL_irq)

add_subdirectory(test)
