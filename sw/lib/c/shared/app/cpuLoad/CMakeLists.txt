add_library(app_cpuLoad STATIC app_cpuLoad.c)
target_include_directories(app_cpuLoad PUBLIC .)
target_link_libraries(app_cpuLoad PUBLIC lib_types lib_timer)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
