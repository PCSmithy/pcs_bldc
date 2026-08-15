add_executable(test_hw_SIL_irq test_SIL_irq.c)
target_link_libraries(test_hw_SIL_irq PRIVATE hw_SIL_irq unity)
add_test(NAME hw_SIL_irq COMMAND test_hw_SIL_irq)
