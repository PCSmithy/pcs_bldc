# Per-target optimisation for the protocol server's byte-work: the sample
# ring, COBS framing, CRC, protobuf, and the USB transport. Measured
# 2026-10-17 (docs/backlog.md, "20 kHz interrupt hot path"): -O2 doubles
# the server's pass rate where the HAL-bound control path barely moves, so
# these targets take PCS_HOT_PATH_OPT_LEVEL on the embedded build and
# everything else keeps PCS_OPT_LEVEL for debuggability. A target option
# follows the directory-level one on the command line, so it wins.
# No-op on the native build.

set(PCS_HOT_PATH_OPT_LEVEL "-O2" CACHE STRING
    "Optimization level for the embedded build's server-path targets")

function(pcs_hot_path target)
  if(PCS_TARGET STREQUAL "embedded")
    target_compile_options(${target} PRIVATE ${PCS_HOT_PATH_OPT_LEVEL})
  endif()
endfunction()
