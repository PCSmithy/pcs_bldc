# Reusable HW_ADC mock: per-(channel, IN#) volts with a readable flag, so a
# consumer can inject sensed voltages and exercise the read-failure path.
# Consumed by any suite that drives a module bottoming out on HW_ADC.
add_library(mock_hw_ADC STATIC mock_HW_ADC.c)
target_include_directories(mock_hw_ADC PUBLIC .)
target_link_libraries(mock_hw_ADC PUBLIC lib_types)
