# C++ Migration Plan

Status: **planned, not started.** Baselines in this document were measured on
2026-09-28 against `main` at `ff5aa3b`.

## Motivation

The firmware is C today. The goal of introducing C++ is to build direct
experience writing embedded C++ — not to unlock a capability that C cannot
provide. The channelization pattern already gives us multiple independent
instances of a driver with their own state, so nothing here is blocked on the
language.

What C++ is expected to buy, in rough order of value to this project:

- `constexpr` tables computed at compile time and placed in flash (SVM sector
  maps, sine tables, pole-pair-derived constants).
- Strong types for the angle domains. `magneticAngle_rad` (electrical) and the
  mechanical quantities next to it are both `float32_t` today; distinct types
  make the classic electrical/mechanical mix-up a compile error.
- Static polymorphism — a PLL or observer templated on its loop-filter policy,
  rather than a runtime-dispatched interface. No vtable in a PWM ISR.
- RAII where it genuinely pays: a scoped critical-section guard around
  `taskENTER_CRITICAL` / `taskEXIT_CRITICAL`, replacing paired calls.
- `constinit` to have the compiler prove an object needs no dynamic
  initialization, which matters because constructors run out of
  `__libc_init_array` before `main`.

Deliberately out of scope: exceptions, RTTI, the heap, `<iostream>`,
`std::function`, `std::string`. With `-fno-exceptions`, `std::array::at()` is
also off the table — use `operator[]`.

`IO_bridge` is the first module to convert. It is a small leaf with an existing
Unity suite and SIL coverage, so it exercises the toolchain plumbing without
risking much. `app_motorControl` follows once the plumbing is proven.

## Verified facts

Measured, not assumed:

| Fact | Result |
|---|---|
| `arm-none-eabi-g++` present | 14.3.1 (GNU Tools for STM32 14.3.rel1), alongside the gcc already in use |
| Current ARM image size | `pcs_bldc_fw.bin` = 91,216 bytes of 128 KB flash (~71%) |
| OFT baseline | `oft.sh trace specs/ sw/ README.md` → 1003 total, 28 defect |
| OFT scans `.cpp` / `.hpp` | Yes. A probe file with `[impl->fw~io_bridge_001~1]` in both extensions resolved. No OFT config change needed. |
| CMake linker-language inference | A C executable linking a static library with C++ sources is inferred `LINKER_LANGUAGE=CXX` and linked with `g++`. Setting the property explicitly is insurance, not a requirement. |
| C++-linkage function pointer → C callback typedef | Compiles clean, no diagnostic even under `-Wall -Wextra -Wpedantic`. Not a blocker for `HW_ADC_registerInjectedCallback`. |
| C99 **array** designated initializers under C++ | `-Wpedantic` warning in both `-std=c++20` and `-std=gnu++20`. GCC accepts them as an extension; setting `CMAKE_CXX_EXTENSIONS OFF` does not turn it into an error. Must be rewritten to keep the build warning-clean. |

Startup and link infrastructure is already C++-ready and needs no change:
`startup_stm32g431vbtx.s:99` calls `__libc_init_array` before `main`, and
`STM32G431VBTX_FLASH.ld:89-130` already carries `.preinit_array`,
`.init_array`, `.fini_array`, `.ARM.exidx` and `.ARM.extab`.

Both toolchain files already locate and `set()` a C++ compiler
(`arm-none-eabi.cmake:40`, `native.cmake:24`); the setting is currently inert
because no `project()` enables the language. Both apply flags through
`add_compile_options` rather than `CMAKE_C_FLAGS`, so the MCU flags
(`-mcpu`, `-mfpu`, `-mfloat-abi=hard`, `-mthumb`) and the warning set extend to
C++ automatically — there is no route to a silent float-ABI mismatch between
the two languages.

CI needs no change: the Linux and macOS runners already provide `g++`, the
Windows `mingw-w64-x86_64-gcc` package ships it, and the
`arm-none-eabi-gcc-action` toolchain includes `arm-none-eabi-g++`. There is no
`.clang-format` in this repo, so no formatter configuration to extend.

## Step 0 — the constraint that shapes the design

The SIL harness reads firmware state by **literal DWARF member path**, not
through an exported accessor. `dwarf_map` resolves names from `DW_AT_name`
only (`sw/lib/rust/dwarf_map/src/lib.rs:688`).

`sw/sil/pcs_bldc_sil/tests/injected_seam.rs:20-29` depends on these paths:

```
IO_bridge_data.channels[0].current_amps[N]
IO_bridge_data.channels[0].updateCount[N]
IO_bridge_data.channels[0].sampleTime_us[N]
```

So the file-scope object must keep the name `IO_bridge_data` and that member
shape. Moving it into a namespace, or making it a class static member, changes
or removes that path and breaks those tests **silently** — the lookup fails at
runtime, not at compile time.

Decision for this migration: **preserve the path through the mechanical
conversion.** Revisit only in the idiomatic pass, and if the shape changes,
update `injected_seam.rs` in the same commit.

`IO_bridge_channelConfig[0].phaseCurrent[0].zeroCurrentBias_V`, read by
`current_sense_roundtrip.rs:110,114`, lives in
`sw/fw/src/io/bridge/IO_bridge_channels.c`. That file stays C, so it is
unaffected.

## Phase A — enable CXX with zero `.cpp` files

Proves the build-system change in isolation.

1. `sw/fw/CMakeLists.txt:9` → `project(pcs_bldc_fw LANGUAGES C CXX ASM)`.
2. `sw/lib/c/CMakeLists.txt:20` → `project(pcs_lib_c LANGUAGES C CXX ASM)`.
3. Alongside the existing C standard block in both files:
   ```cmake
   set(CMAKE_CXX_STANDARD          20)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)
   set(CMAKE_CXX_EXTENSIONS        OFF)
   ```
   C++20, not 23 — GCC 14 implements 20 completely and 23 only in part.
4. In both toolchain files, add the C++-only flags via generator expression so
   they cannot leak onto C translation units:
   ```cmake
   add_compile_options(
     $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)
   ```
   `-fno-threadsafe-statics` removes the `__cxa_guard_acquire` calls GCC emits
   around function-local statics; `-fno-use-cxa-atexit` stops it registering
   static destructors that never run on this target anyway.

Gate: all three builds pass and the ARM image is still 91,216 bytes. If flash
moved, something in step 4 leaked onto the C build.

## Phase B — `extern "C"` the boundary headers, still all C

The blast radius is wider than `IO_bridge.h` alone. Once `IO_bridge.cpp`
exists it will include `HW_TIM.h` and `HW_ADC.h` as a C++ translation unit and
emit **mangled references** to `HW_TIM_setCompare`, `HW_ADC_getVolts` and the
rest, which will not resolve against the C-compiled archives.

Headers that need the guard:

| Header | Reason |
|---|---|
| `sw/lib/c/shared/io/bridge/IO_bridge.h` | consumed by `sw/fw/src/main.c:28`, `sw/fw/src/io/bridge/IO_bridge_channels.c:2`, `sw/lib/c/shared/app/motorControl/app_motorControl.h:6` (and so every transitive C includer), `.../io/bridge/test/test_IO_bridge.c:1` |
| `sw/lib/c/shared/hw/TIM/HW_TIM.h` | included *by* the new `.cpp` |
| `sw/lib/c/shared/hw/ADC/HW_ADC.h` | same, plus the `HW_ADC_callback_F` typedef |
| `sw/lib/c/shared/test/mocks/hw/TIM/HW_TIM.h` | the shadow header both unit-test harnesses compile the driver against |
| `sw/lib/c/shared/test/mocks/hw/ADC/HW_ADC.h` | same |

Standard wrapper around the declarations:

```c
#ifdef __cplusplus
extern "C" {
#endif
...
#ifdef __cplusplus
}
#endif
```

**Not** required: any of the three `IO_bridge_channels.h` copies
(`sw/fw/src/io/bridge/`, `sw/lib/c/shared/io/bridge/test/`,
`sw/lib/c/shared/app/motorControl/test/`). They define only an enum; no
linkage is involved.

Gate: still a pure-C build, all three builds and both unit-test harnesses
unchanged.

## Phase C — rename to `.cpp`

5. `git mv sw/lib/c/shared/io/bridge/IO_bridge.c IO_bridge.cpp`.
6. Update **three** CMakeLists, not one:
   - `sw/lib/c/shared/io/bridge/CMakeLists.txt` — `add_library(io_bridge STATIC IO_bridge.cpp)`.
   - `sw/lib/c/shared/io/bridge/test/CMakeLists.txt` — `${CMAKE_CURRENT_SOURCE_DIR}/../IO_bridge.cpp`.
   - `sw/lib/c/shared/app/motorControl/test/CMakeLists.txt` —
     `${CMAKE_CURRENT_SOURCE_DIR}/../../../io/bridge/IO_bridge.cpp`. Easy to
     miss: that harness compiles the real `IO_bridge` alongside
     `app_motorControl.c`, so it becomes a mixed-language link too.
7. Fix what C++ rejects. In this file the list is short:
   - `IO_bridge.c:35-40` — `IO_bridge_complementaryPhase` uses C99 **array**
     designated initializers (`[IO_BRIDGE_PHASE_U] = ...`). C++20's designated
     initializers cover aggregates, never array subscripts. Rewrite
     positionally; the existing entries are already in U, V, W order, so it is
     a mechanical swap.
   - The `(HW_TIM_peripheral_E)0` initializers at `IO_bridge.c:235-237` are
     already explicit casts and stay valid. C++ forbids *implicit* int-to-enum
     conversion; this file never relies on it, which is most of why the
     conversion is cheap.
   - `IO_bridge_data` and `data` are constant-initialized, so there is no
     static-initialization-order exposure. Optionally mark them `constinit` to
     have that checked.
   - `IO_bridge_private_injectedComplete` (`IO_bridge.c:126`, registered at
     `:199-201`) can stay as it is. Giving it `extern "C"` is the
     standards-pure choice and documents that it runs in ISR context.
8. Leave the five `[impl->fw~io_bridge_00N~1]` tags where they are
   (`IO_bridge.c:218, :279, :304, :322, :337`).

Commit at this point with **structure, names and behavior identical** — the
only diff is language mechanics. The 20 `[test->]`-tagged assertions in
`test_IO_bridge.c` plus the SIL injected-seam tests are the oracle.

## Phase D — the actual C++ (separate commit)

Only now introduce idioms: a `Bridge` type holding what
`IO_bridge_channelData_S` holds, `std::array` over the raw arrays, `constexpr`
for the complementary-phase table, a scoped enum for `IO_bridge_phase_E`.

Keep `IO_bridge_data` at namespace scope under that name so the SIL DWARF paths
survive, or change the shape and update `injected_seam.rs` deliberately in the
same commit. See Step 0.

Separating the language change from the design change is both good practice and
better pedagogy: the Phase D diff shows exactly what each idiom cost or saved in
flash.

## Verification gate

Run after every phase:

```bash
tools/build_native.sh          # includes ctest
tools/build_arm.sh             # compare --print-memory-usage against 91,216 B
tools/run_sil.sh
tools/oft/oft.sh trace specs/ sw/ README.md   # expect 1003 total, 28 defect
```

## Open questions

- Whether `docs/c-coding-conventions.md` grows a C++ section or gets a sibling
  document. Naming in particular needs a ruling: the existing scheme is
  `IO_bridge_setPhaseDuty` with a `_private_` infix, which does not map cleanly
  onto classes and member functions.
- Whether `app_motorControl` converts before or after the SVM and PLL work
  lands. Converting first means writing the new control code in C++ from the
  start; converting after means a larger, riskier mechanical pass.
