add_library(lib_ringbuf STATIC
  ringbuf.c
)
target_include_directories(lib_ringbuf PUBLIC .)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
