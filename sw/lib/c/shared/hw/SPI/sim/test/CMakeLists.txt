add_executable(test_hw_SPI test_HW_SPI.c)
target_link_libraries(test_hw_SPI PRIVATE hw_SPI hw_SIL_irq_double unity)
add_test(NAME hw_SPI COMMAND test_hw_SPI)
