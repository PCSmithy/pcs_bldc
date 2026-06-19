add_executable(test_hw_ADC test_HW_ADC.c)
target_link_libraries(test_hw_ADC PRIVATE hw_ADC unity)
add_test(NAME hw_ADC COMMAND test_hw_ADC)
