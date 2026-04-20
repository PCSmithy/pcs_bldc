# pcs_bldc firmware executable.

add_subdirectory(hw)
# Layer subdirs (app/, dev/, io/, lib/) are not yet populated; uncomment
# their add_subdirectory() lines when their first code lands.

add_executable(pcs_bldc_fw main.c)

target_include_directories(pcs_bldc_fw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pcs_bldc_fw PRIVATE fw_hw)

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
