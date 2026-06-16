add_library(io_usb STATIC usb.c usb_descriptors.c)
target_include_directories(io_usb PUBLIC .)
target_link_libraries(io_usb PUBLIC tinyusb freertos stm32g4_hal)
