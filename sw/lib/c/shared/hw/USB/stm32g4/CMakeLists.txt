# Embedded HW_USB: TinyUSB device stack + CDC + the device/string descriptors.
# tinyusb's usbd.c calls our tud_descriptor_*_cb (in HW_USB_descriptors.c) while
# we call tud_*; that cycle is resolved by the --start-group at the executable
# link (see sw/fw/src/CMakeLists.txt).
add_library(hw_USB STATIC HW_USB.c HW_USB_descriptors.c)
target_include_directories(hw_USB PUBLIC .)
target_link_libraries(hw_USB
  PUBLIC
    lib_types
    tinyusb        # tusb.h + the device stack
    freertos       # the device-service task
    stm32g4_hal    # USB clock routing + NVIC
)
