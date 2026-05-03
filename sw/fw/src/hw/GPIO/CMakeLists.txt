# GPIO has no project-side extension header (the port enum is HW-fixed
# for the whole G4 family and lives library-side in HW_GPIO.h), so no
# pcs_GPIO_channels INTERFACE lib is needed. The project still owns
# the HW_GPIO_config instance via HW_GPIO_channels.c.
target_sources(fw_hw PRIVATE HW_GPIO_channels.c)
target_link_libraries(fw_hw PUBLIC hw_GPIO lib_build lib_utils)
