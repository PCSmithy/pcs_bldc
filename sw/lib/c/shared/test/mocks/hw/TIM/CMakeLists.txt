# Reusable HW_TIM mock: records per-channel compare/output-enable, per-
# peripheral MOE, and break-flag clears. Consumed by any suite that drives a
# module bottoming out on HW_TIM (io_bridge, app_motorControl, ...).
add_library(mock_hw_TIM STATIC mock_HW_TIM.c)
target_include_directories(mock_hw_TIM PUBLIC .)
target_link_libraries(mock_hw_TIM PUBLIC lib_types)
