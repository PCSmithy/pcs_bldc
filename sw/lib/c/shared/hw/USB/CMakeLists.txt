# HW_USB: USB device peripheral + TinyUSB device-stack lifecycle + CDC byte
# interface. Dual-target: the real TinyUSB stack on stm32g4, a loopback on sim
# (which backs IO_serial's native unit tests). Single-instance — no project
# channel/config seam (there is one USB peripheral).
if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(stm32g4)
else()
  add_subdirectory(sim)
endif()
