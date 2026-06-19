# AS5048 channel config is dual-target (its driver only needs hw_SPI). usb
# (tinyusb) and SK6805 stay embedded-only.
add_subdirectory(AS5048)
if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(usb)
  add_subdirectory(SK6805)
endif()
