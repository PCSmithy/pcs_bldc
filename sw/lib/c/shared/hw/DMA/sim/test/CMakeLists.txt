add_executable(test_hw_DMA test_HW_DMA.c)
target_link_libraries(test_hw_DMA PRIVATE hw_DMA unity)
add_test(NAME hw_DMA COMMAND test_hw_DMA)
