# HW_systemClock_config.c holds the pcs_bldc-specific channel instance.
# It compiles into fw_hw (which is STATIC on both targets — see the sim
# CMakeLists for why), and the transitive deps on hw_systemClock +
# lib_build propagate up to pcs_bldc_fw via PUBLIC.
target_sources(fw_hw PRIVATE HW_systemClock_config.c)
target_link_libraries(fw_hw PUBLIC hw_systemClock lib_build)
