# pcs_bldc firmware: an executable (.elf) on embedded, a shared library (.dll)
# on native (SIL) that the Rust framework loads and drives.

add_subdirectory(hw)
# lib/ holds project-side configs for cross-cutting lib modules (e.g.
# lib_timer_config). Added after hw/ so its sources can attach to fw_hw.
add_subdirectory(lib)
# io/ has dual-target modules (io_AS5048) and embedded-only ones (usb, SK6805);
# the per-module gating lives in io/CMakeLists.txt. dev/ (switch debounce) and
# app/ (rgbLedRing UI) are target-independent and build for both targets; each
# provides a project-side channel-config seam consumed by the shared module.
add_subdirectory(io)
add_subdirectory(dev)
add_subdirectory(app)

# Native (SIL): a SHARED library the Rust framework dlopen's and drives via the
# control ABI (D2). --export-all-symbols exposes sil_fw_* (and globals, for
# white-box symbol access). main()'s SIM smoke driver is unused inside the DLL.
if(PCS_TARGET STREQUAL "native")
  add_library(pcs_bldc_fw SHARED main.c)
  target_link_options(pcs_bldc_fw PRIVATE -Wl,--export-all-symbols)
else()
  add_executable(pcs_bldc_fw main.c)
endif()

target_include_directories(pcs_bldc_fw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# fw_hw carries the vector-table ISR handlers (stm32g4xx_it.c: SysTick, USB_LP,
# EXTI, ...). These are strong symbols meant to override the weak
# Default_Handler aliases in startup.s. With normal static-archive linking the
# linker never pulls it.o — nothing references an *undefined* symbol it
# provides — so every ISR silently falls back to the Default_Handler infinite
# loop (SysTick never ticks -> HAL_Delay hangs; USB/EXTI dead). --whole-archive
# forces every fw_hw object to be linked so the real handlers win. fw_hw stays a
# target (not a bare flag), so its usage requirements — include dirs and the
# linker script / --specs via INTERFACE link options — still propagate.
target_link_libraries(pcs_bldc_fw PRIVATE
  -Wl,--whole-archive
  fw_hw
  -Wl,--no-whole-archive
)

# newlib (--specs=nano.specs) is appended by the compiler driver after all of
# our explicit libraries, and references the libc syscall backend (_sbrk,
# _write, _read, ...) defined in hw_stm32g4 (syscalls.c / sysmem.c). With plain
# ordering those archive members were already passed before libc demanded them,
# so they went unresolved as soon as printf (via io_usb) pulled stdio in. Force
# hw_stm32g4 in so the syscall stubs are always present for libc to bind to.
# (It's also the SystemInit provider, which must be present regardless.)
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE
    -Wl,--whole-archive
    hw_stm32g4
    -Wl,--no-whole-archive
  )
endif()

# io_serial: the CDC byte-stream transport. Builds for both targets — it talks
# to its backing transport only through the HW_USB CDC API, which is the real
# TinyUSB stack on embedded and a loopback on native. io_serial_channels pulls in
# io_serial, which PUBLIC-links hw_USB, so the HW_USB_* references resolve
# transitively on either target. Link it before the USB group below so those
# references are already pending when hw_USB is pulled.
target_link_libraries(pcs_bldc_fw PRIVATE io_serial_channels)

# USB device stack (embedded only). hw_USB and tinyusb have a genuine cyclic
# dependency: tinyusb's usbd.c calls our tud_descriptor_*_cb (in hw_USB's
# descriptor unit), while hw_USB calls tinyusb's tud_* API. A single-pass static
# link can't satisfy a cycle, so wrap them in an ld --start-group/--end-group
# that re-scans the pair until no new references resolve. fw_hw's
# USB_LP_IRQHandler also references dcd_int_handler (tinyusb); fw_hw is scanned
# before the group, and the group resolves that pending reference too. On native
# there is no TinyUSB stack — io_serial's transitive hw_USB is the sim loopback,
# a plain acyclic archive that needs no group.
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE
    -Wl,--start-group
    hw_USB
    tinyusb
    -Wl,--end-group
  )
endif()

# io/dev/app modules: the AS5048 encoder, SK6805 LED string, I2C device driver,
# switch debounce, CYPD3177 PD sink, and LED-ring UI. All are target-independent
# and build for both targets. Each links its own hw_* dependency PUBLIC
# (io_AS5048/io_SK6805 -> hw_SPI, io_i2c -> hw_I2C, dev_switch -> hw_GPIO), so the
# HW_* references resolve against the target's HW drivers — the real ones on
# embedded, the sim drivers on native — pulled in transitively.
# app_rgbLedRing_channels transitively pulls the io/dev modules it composes;
# dev_CYPD3177_channels pulls io_i2c + lib_CYPD3177; dev_gateDriver_channels
# pulls io_i2c.
target_link_libraries(pcs_bldc_fw PRIVATE
  io_AS5048_channels
  io_SK6805_channels
  io_i2c_channels
  dev_switch_channels
  dev_CYPD3177_channels
  dev_gateDriver_channels
  app_rgbLedRing_channels
)

# Embedded-target post-build: emit .bin and .hex artifacts and print the
# memory section sizes (text/data/bss + flash/RAM percentages from the
# linker's --print-memory-usage in fw_hw's link options).
if(PCS_TARGET STREQUAL "embedded")
  set_target_properties(pcs_bldc_fw PROPERTIES SUFFIX ".elf")
  add_custom_command(TARGET pcs_bldc_fw POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:pcs_bldc_fw> $<TARGET_FILE_DIR:pcs_bldc_fw>/pcs_bldc_fw.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:pcs_bldc_fw> $<TARGET_FILE_DIR:pcs_bldc_fw>/pcs_bldc_fw.hex
    COMMAND ${CMAKE_SIZE}    $<TARGET_FILE:pcs_bldc_fw>
    VERBATIM
  )
endif()
