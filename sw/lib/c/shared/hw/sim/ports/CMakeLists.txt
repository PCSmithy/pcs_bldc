add_library(hw_SIL_ports STATIC SIL_ports.c)
target_include_directories(hw_SIL_ports PUBLIC .)
target_link_libraries(hw_SIL_ports
  PUBLIC
    lib_types
)

# Unit tests run only on the native target (this whole tree is native-only,
# but keep the guard for symmetry with the other hw modules).
if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
