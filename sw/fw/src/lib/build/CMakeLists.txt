# lib_build's project side: the generated build identity
# (fw~obs_identity_001). The custom target runs every build; the header
# is only rewritten when the identity actually changes.
set(PCS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_custom_target(pcs_build_identity
  COMMAND ${CMAKE_COMMAND}
    -DREPO_ROOT=${PCS_REPO_ROOT}
    -DOUT=${CMAKE_CURRENT_SOURCE_DIR}/generated/lib_build_identity.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/generate_identity.cmake
  BYPRODUCTS ${CMAKE_CURRENT_SOURCE_DIR}/generated/lib_build_identity.h
  COMMENT "build identity: refreshing lib_build_identity.h"
  VERBATIM
)

# STATIC (not INTERFACE): lib_build_identity.c compiles the identity into a
# named, DWARF-visible object every consumer links.
add_library(lib_build_config STATIC lib_build_identity.c)
target_include_directories(lib_build_config PUBLIC . generated)
target_link_libraries(lib_build_config PUBLIC lib_types)
add_dependencies(lib_build_config pcs_build_identity)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
