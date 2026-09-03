# lib_timer's project-side config: binds the software-timer base to HW_TIM
# (TIM2, the 1 us free-running counter). Compiled into fw_hw; lib_timer.c
# externs the lib_timer_config it defines. Depends on lib_timer (the config
# struct type) and hw_TIM (HW_TIM_getCounter).
target_sources(fw_hw PRIVATE lib_timer_config.c)
target_link_libraries(fw_hw PUBLIC lib_timer hw_TIM freertos)
