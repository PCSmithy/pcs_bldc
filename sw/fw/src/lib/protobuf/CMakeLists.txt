# lib_protobuf's project side: the nanopb-generated bindings for the
# sw/proto/ schema (encode/decode goes through the generic lib_protobuf).
#
# generated/*.pb.{h,c} are generated (never committed) by a custom command
# whenever the schema changes. The project venv is a hard build requirement
# here — ./setup.sh creates it. tools/generate_proto.sh is the standalone
# manual path.
set(PCS_BOARD_PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../proto)
set(PCS_SHARED_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../lib/c/shared/proto)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/shared.pb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/shared.pb.h
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/trace.pb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/trace.pb.h
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/board.pb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/board.pb.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/generated
  COMMAND ${PCS_VENV_PY} -m nanopb.generator.nanopb_generator
    --proto-path=${PCS_SHARED_PROTO_DIR}
    --proto-path=${PCS_BOARD_PROTO_DIR}
    --output-dir=${CMAKE_CURRENT_SOURCE_DIR}/generated
    shared.proto trace.proto board.proto
  DEPENDS
    ${PCS_SHARED_PROTO_DIR}/shared.proto
    ${PCS_SHARED_PROTO_DIR}/shared.options
    ${PCS_SHARED_PROTO_DIR}/trace.proto
    ${PCS_SHARED_PROTO_DIR}/trace.options
    ${PCS_BOARD_PROTO_DIR}/board.proto
  COMMENT "nanopb: regenerating lib/protobuf/generated/{shared,trace,board}.pb.{h,c}"
  VERBATIM
)

add_library(lib_protobuf_config STATIC
  generated/shared.pb.c
  generated/trace.pb.c
  generated/board.pb.c
)
target_include_directories(lib_protobuf_config PUBLIC . generated)
target_link_libraries(lib_protobuf_config PUBLIC lib_types nanopb)

if(PCS_TARGET STREQUAL "native")
  add_subdirectory(test)
endif()
