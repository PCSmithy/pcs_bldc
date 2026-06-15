# pcs_bldc firmware executable.

add_subdirectory(hw)
# Layer subdirs (app/, dev/, io/, lib/) are not yet populated; uncomment
# their add_subdirectory() lines when their first code lands.

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
