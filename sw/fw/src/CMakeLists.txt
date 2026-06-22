# pcs_bldc firmware executable.

add_subdirectory(hw)
# lib/ holds project-side configs for cross-cutting lib modules (e.g.
# lib_timer_config). Added after hw/ so its sources can attach to fw_hw.
add_subdirectory(lib)
# Remaining layer subdirs (app/, dev/) populate when their first code lands.

# io/ has dual-target modules (io_AS5048) and embedded-only ones (usb, SK6805);
# the per-module gating lives in io/CMakeLists.txt. dev/ (switch debounce) is
# target-independent and builds for both targets.
add_subdirectory(io)
add_subdirectory(dev)

add_executable(pcs_bldc_fw main.c)

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

# hw_USB (embedded) + io_serial. hw_USB and tinyusb have a genuine cyclic
# dependency: tinyusb's usbd.c calls our tud_descriptor_*_cb (in hw_USB's
# descriptor unit), while hw_USB calls tinyusb's tud_* API. A single-pass static
# link can't satisfy a cycle, so wrap them in an ld --start-group/--end-group
# that re-scans the pair until no new references resolve. io_serial (pulled in by
# io_serial_channels) layers the byte stream on hw_USB; link it before the group
# so its HW_USB_* references resolve when hw_USB is pulled. fw_hw's
# USB_LP_IRQHandler also references dcd_int_handler (tinyusb); fw_hw is scanned
# before the group, and the group resolves that pending reference too.
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE
    io_serial_channels
    -Wl,--start-group
    hw_USB
    tinyusb
    -Wl,--end-group
  )
endif()

# io_AS5048 (embedded only): the AS5048 encoder driver + its project channel
# config (io_AS5048_channels pulls in io_AS5048). Their HW_SPI_* references
# resolve against fw_hw (whole-archived above), so a plain PRIVATE link after
# fw_hw is sufficient.
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE io_AS5048_channels)
endif()

# io_SK6805 (embedded only): the SK6805 LED-string driver + its project config
# (io_SK6805_config pulls in io_SK6805), same linking story as io_AS5048.
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE io_SK6805_channels)
endif()

# dev_switch: the driver builds for both targets (native Unity suite included),
# but only the embedded executable consumes it (the SIM main() has no UI yet).
# dev_switch_channels pulls in dev_switch; HW_GPIO_* resolve via fw_hw.
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE dev_switch_channels)
endif()

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
