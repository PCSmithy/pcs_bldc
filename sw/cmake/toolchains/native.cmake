# Native (host) toolchain — used for SIL builds and unit tests.
#
# Goals:
#   - Use the host gcc (from MinGW on Windows, system gcc on macOS).
#   - Stay in the GCC family for parity with the embedded toolchain
#     (matches diagnostics, language semantics, sanitizer behavior).
#   - Be intentionally simple: no sanitizers in the default build (they're
#     useful but flaky on some MinGW setups). Add via -DPCS_SANITIZE=ON
#     once we know we want them.

set(PCS_TARGET "native" CACHE STRING "Build target identifier")

# Preprocessor-visible build target. Source files use
#   #if (BUILD_TARGET == BUILD_TARGET_SIM) ...
# with the named constants defined in sw/lib/c/shared/lib/build/lib_build.h.
add_compile_definitions(BUILD_TARGET=BUILD_TARGET_SIM)

# Find host GCC explicitly so CMake doesn't auto-pick MSVC on Windows when
# invoked outside a Visual Studio dev shell.
find_program(NATIVE_C   gcc REQUIRED)
find_program(NATIVE_CXX g++ REQUIRED)

set(CMAKE_C_COMPILER   "${NATIVE_C}")
set(CMAKE_CXX_COMPILER "${NATIVE_CXX}")

# Optimization level. -O0 is the default: it keeps the dev/test flow (Unity,
# debugging) source-faithful. Override at configure time without editing this
# file (mirrors the embedded toolchain's PCS_OPT_LEVEL knob):
#   -DPCS_OPT_LEVEL=-O3   optimized (SIL performance runs; tools/run_sil.sh default)
# -g is ALWAYS kept: the SIL framework reads firmware statics by DWARF, so debug
# info must survive at every opt level (statics stay exported via
# --export-all-symbols, so the optimizer cannot eliminate them either).
set(PCS_OPT_LEVEL "-O0" CACHE STRING "Optimization level for the native build")

# Link-time optimization (release SIL DLL only). OFF by default so the -O0
# dev/test flow is untouched. When ON, -flto is added to BOTH compile and link
# (LTO needs it in both places), and -ffat-lto-objects keeps a real machine-code
# .o alongside the GIMPLE bytecode — required so the SHARED-library link's
# -Wl,--whole-archive fw_hw and -Wl,--export-all-symbols still see concrete
# symbols (the plugin can't export from bytecode-only objects), and so the
# DWARF-read firmware statics survive. No -ffast-math: FP semantics unchanged.
set(PCS_LTO OFF CACHE BOOL "Enable link-time optimization (release SIL DLL)")

add_compile_options(
  -Wall -Wextra -Wpedantic
  -g ${PCS_OPT_LEVEL}
)

# The firmware links as a SHARED library (SIL). On Linux/ELF that requires all
# objects — including the static libs it pulls in — to be position-independent,
# or the .so link fails with "final link failed: bad value". Harmless on macOS
# (already PIC) and MinGW (ignored).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(PCS_LTO AND CMAKE_HOST_WIN32)
  # LTO is gated to Windows (MinGW/GNU) ONLY — the one platform where it is
  # verified end-to-end (the -O3 -flto -ffat-lto-objects DLL loads and its DWARF
  # reads clean). Evidence over hope for the other two hosts:
  #   - Linux (GNU): the -flto ELF .so links, but gimli parses ZERO DIEs from it
  #     (empty DWARF map -> the SIL reader finds no ASLR anchor / statics). See
  #     docs/sil/backlog.md ("investigate GCC LTO DWARF emission on ELF").
  #   - macOS: `gcc` is Apple clang, which has neither -ffat-lto-objects nor the
  #     GCC LTO plugin model, so the fat-object trick does not apply.
  # So Linux/macOS release keeps -O3 (no -flto); only Windows adds LTO. The gate
  # is host-level (CMAKE_HOST_WIN32 is known at toolchain-parse time, and the
  # native toolchain targets the host); the $<...:GNU> genexp still guards the
  # flags so a non-GNU driver never sees them. -ffat-lto-objects keeps concrete
  # machine-code symbols so the SHARED library's -Wl,--whole-archive fw_hw /
  # -Wl,--export-all-symbols and the DWARF-read statics survive LTO. -flto on
  # compile and link (LTO needs both); no -ffast-math, so FP semantics unchanged.
  add_compile_options(
    $<$<C_COMPILER_ID:GNU>:-flto>
    $<$<C_COMPILER_ID:GNU>:-ffat-lto-objects>
  )
  # Pass the opt level and -g at link too so the LTO code-gen pass runs at the
  # same level and emits debug info for the merged program.
  add_link_options(
    $<$<C_COMPILER_ID:GNU>:-flto>
    ${PCS_OPT_LEVEL} -g
  )
endif()
