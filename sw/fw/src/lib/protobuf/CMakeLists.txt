# lib_protobuf's project side: the nanopb-generated bindings for the
# sw/proto/ schema (encode/decode goes through the generic lib_protobuf).
#
# generated/*.pb.{h,c} are generated (never committed) by a custom command
# whenever the schema changes. The project venv is a hard build requirement
# here — ./setup.sh creates it. tools/generate_proto.sh is the standalone
# manual path.
set(PCS_PROTO_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../proto)
set(PCS_REPO_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
if(EXISTS ${PCS_REPO_ROOT}/.venv/Scripts/python.exe)
  set(PCS_VENV_PY ${PCS_REPO_ROOT}/.venv/Scripts/python.exe)
elseif(EXISTS ${PCS_REPO_ROOT}/.venv/bin/python)
  set(PCS_VENV_PY ${PCS_REPO_ROOT}/.venv/bin/python)
else()
  message(FATAL_ERROR "lib_protobuf_config: no project venv found at ${PCS_REPO_ROOT}/.venv - "
                      "the firmware build generates protocol bindings with the venv's "
                      "nanopb generator. Run ./setup.sh first.")
endif()

add_custom_command(
  OUTPUT
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/pcs_bldc.pb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/pcs_bldc.pb.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/generated
  COMMAND ${PCS_VENV_PY} -m nanopb.generator.nanopb_generator
    --proto-path=${PCS_PROTO_SCHEMA_DIR}
    --output-dir=${CMAKE_CURRENT_SOURCE_DIR}/generated
    pcs_bldc.proto
  DEPENDS
    ${PCS_PROTO_SCHEMA_DIR}/pcs_bldc.proto
    ${PCS_PROTO_SCHEMA_DIR}/pcs_bldc.options
  COMMENT "nanopb: regenerating lib/protobuf/generated/pcs_bldc.pb.{h,c} from sw/proto/"
  VERBATIM
)

add_library(lib_protobuf_config STATIC
  generated/pcs_bldc.pb.c
)
target_include_directories(lib_protobuf_config PUBLIC . generated)
target_link_libraries(lib_protobuf_config PUBLIC lib_types nanopb)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
