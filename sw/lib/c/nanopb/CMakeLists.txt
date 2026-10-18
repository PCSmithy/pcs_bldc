# nanopb 0.4.9.1 — vendored protocol-buffer runtime
# (https://github.com/nanopb/nanopb). Small static-allocation protobuf
# encode/decode for embedded C; builds for both targets. Generated
# message definitions (*.pb.c/.pb.h) live with their consumer — this
# target provides only the runtime.

add_library(nanopb STATIC
  pb_common.c
  pb_encode.c
  pb_decode.c
)

target_include_directories(nanopb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
