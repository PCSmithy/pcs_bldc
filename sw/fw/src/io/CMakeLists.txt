# AS5048 and SK6805 channel configs are dual-target (their drivers only need
# hw_SPI). usb (tinyusb) stays embedded-only.
add_subdirectory(AS5048)
add_subdirectory(SK6805)
add_subdirectory(serial)
add_subdirectory(i2c)
add_subdirectory(bridge)
