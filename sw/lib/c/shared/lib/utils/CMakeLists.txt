add_library(lib_utils INTERFACE)
target_include_directories(lib_utils INTERFACE .)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
