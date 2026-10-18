# HW_CRC_config.c holds the pcs_bldc-specific instance; it compiles into
# fw_hw and its deps propagate up to pcs_bldc_fw via PUBLIC.
target_sources(fw_hw PRIVATE HW_CRC_config.c)
target_link_libraries(fw_hw PUBLIC hw_CRC)
