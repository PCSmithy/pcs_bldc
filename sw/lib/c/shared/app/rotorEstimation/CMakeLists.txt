
add_library(app_rotorEstimation STATIC
  app_rotorEstimation.cpp
)
target_include_directories(app_rotorEstimation PUBLIC .)
target_link_libraries(app_rotorEstimation
  PUBLIC
    lib_types
    lib_utils
    lib_transforms
    lib_matrix
    io_AS5048
    pcs_rotorEstimation_channels
)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
