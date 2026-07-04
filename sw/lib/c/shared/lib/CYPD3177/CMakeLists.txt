add_library(lib_CYPD3177 STATIC
  CYPD3177.c
)
target_include_directories(lib_CYPD3177 PUBLIC .)
target_link_libraries(lib_CYPD3177 PUBLIC lib_types)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
