# pcs_bldc firmware executable.

add_subdirectory(hw)
# Layer subdirs (app/, dev/, io/, lib/) are not yet populated; uncomment
# their add_subdirectory() lines when their first code lands.

# io/ (USB CDC, TinyUSB) is embedded-only; native builds must stay USB-free.
if(PCS_TARGET STREQUAL "embedded")
  add_subdirectory(io)
endif()

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

# io_usb (embedded only). io_usb and tinyusb have a genuine cyclic dependency:
# tinyusb's usbd.c calls our tud_descriptor_*_cb (in io_usb), while io_usb's
# usb.c calls tinyusb's tud_* API. A single-pass static link can't satisfy a
# cycle (whichever archive is scanned first leaves the other's references
# dangling), so wrap them in an ld --start-group/--end-group that re-scans the
# pair until no new references resolve. This avoids forcing --whole-archive on
# io_usb (we don't want every member unconditionally; --gc-sections still drops
# the unused ones). fw_hw's USB_LP_IRQHandler also references dcd_int_handler
# (tinyusb); fw_hw is scanned before the group, and the group resolves that
# pending reference when tinyusb is pulled.
if(PCS_TARGET STREQUAL "embedded")
  target_link_libraries(pcs_bldc_fw PRIVATE
    -Wl,--start-group
    io_usb
    tinyusb
    -Wl,--end-group
  )
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
