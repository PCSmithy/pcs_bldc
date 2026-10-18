add_library(lib_ringbuf STATIC
  ringbuf.c
)
pcs_hot_path(lib_ringbuf)   # server path: see sw/cmake/pcs_hot_path.cmake
target_include_directories(lib_ringbuf PUBLIC .)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
