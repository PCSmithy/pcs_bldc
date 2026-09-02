# Shared pended-IRQ test double, consumed by the sim driver suites.
add_library(hw_SIL_irq_double STATIC SIL_irq_double.c)
target_include_directories(hw_SIL_irq_double PUBLIC .)
target_link_libraries(hw_SIL_irq_double PUBLIC hw_SIL_irq)

add_executable(test_hw_SIL_irq test_SIL_irq.c)
target_link_libraries(test_hw_SIL_irq PRIVATE hw_SIL_irq unity)
add_test(NAME hw_SIL_irq COMMAND test_hw_SIL_irq)
