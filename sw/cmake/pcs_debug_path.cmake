# Per-target debug optimisation. The embedded build defaults to -O2: measured
# 2026-10-17 (docs/backlog.md, "20 kHz interrupt hot path"), -Og on the
# control path and tasks starved the protocol server of the core it needs
# under drive with 20 kHz watches streaming. Call pcs_debug_path(<target>)
# on the module you are stepping through to build it at
# PCS_DEBUG_PATH_OPT_LEVEL; a target option follows the directory-level one
# on the command line, so it wins. No-op on the native build.

set(PCS_DEBUG_PATH_OPT_LEVEL "-Og" CACHE STRING
    "Optimization level for embedded targets opted into pcs_debug_path()")

function(pcs_debug_path target)
  if(PCS_TARGET STREQUAL "embedded")
    target_compile_options(${target} PRIVATE ${PCS_DEBUG_PATH_OPT_LEVEL})
  endif()
endfunction()
