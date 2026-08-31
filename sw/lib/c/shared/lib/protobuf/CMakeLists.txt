add_library(lib_protobuf STATIC
  lib_protobuf.c
)
target_include_directories(lib_protobuf PUBLIC .)
target_link_libraries(lib_protobuf PUBLIC lib_types nanopb)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
