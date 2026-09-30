# C++ transition plan

Status: **planned, not started.** Baselines measured 2026-09-28 against `main`
at `d0c7376`.

## Motivation and scope

The firmware is C today. C++ becomes the language for new control code and
for every moderately complex module as it is next touched — this is a
transition, not a one-module exercise:

1. `IO_bridge` converts first: a small leaf with a Unity suite and SIL
   coverage, so it proves the plumbing at low risk. The space-vector
   modulator (`fw~mc_014`) then lands in it as C++.
2. `app_motorControl` converts before FOC, so the V/f method (`fw~mc_010`,
   `fw~mc_017`) and the current loop after it are written in C++ from the
   start.
3. New modules — the frame transforms (`fw~mc_013`), `app_rotorEstimation`
   for the rotor PLL — are C++ from their first commit.

What C++ is expected to buy, in rough order of value:

- `constexpr` tables in flash (SVM sector maps, sine tables, pole-pair
  constants).
- Strong types for the angle domains: electrical and mechanical angles are
  both `float32_t` today; distinct types make the mix-up a compile error.
- Static polymorphism — an observer templated on its loop-filter policy
  instead of a runtime-dispatched interface. No vtable in a PWM ISR.
- RAII where it pays: a scoped critical-section guard replacing paired
  `taskENTER_CRITICAL` / `taskEXIT_CRITICAL` calls.
- `constinit` to prove an object needs no dynamic initialization
  (constructors run from `__libc_init_array` before `main`).

Language subset, fixed: no exceptions, no RTTI, no heap, no `<iostream>`, no
`std::function`, no `std::string`. With `-fno-exceptions`,
`std::array::at()` is out — use `operator[]`.

## Verified facts

Measured, not assumed:

| Fact | Result |
|---|---|
| `arm-none-eabi-g++` present | Arm GNU Toolchain 15.2.1 (what `arm-none-eabi.cmake` finds); native MinGW g++ 15.2.0 |
| ARM image at `d0c7376` | 94,304 B flash (72%), 28,920 B RAM (88%) |
| OFT baseline | `oft.sh trace specs/ sw/ README.md` → 1130 total, 34 defects, all in CLAUDE.md's allowed classes |
| OFT scans `.cpp` / `.hpp` | Yes — a probe `[impl->fw~io_bridge_001~1]` resolved in both. No config change |
| CMake linker language | A C executable linking a static library with C++ sources is inferred `LINKER_LANGUAGE=CXX` and linked with `g++`. Setting it explicitly is insurance |
| C++-linkage function pointer → C callback typedef | Compiles clean under `-Wall -Wextra -Wpedantic`. `HW_ADC_registerInjectedCallback` and `IO_bridge_registerCycleCallback` are not blockers |
| C99 **array** designated initializers | `-Wpedantic` warning in C++20 (GCC extension; `CMAKE_CXX_EXTENSIONS OFF` does not make it an error). Rewrite positionally |
| `COUNTOF` (`lib_utils.h`) | Uses `__builtin_types_compatible_p`, which does not exist in C++. Needs a `__cplusplus` branch before any C++ unit uses it |
| Unity | `unity.h` carries `extern "C"` guards; a `.cpp` test file works unchanged |
| `dwarf_map` variables | Collected by bare `DW_AT_name` at any nesting depth: a namespace does not hide a variable, but two namespaces with the same variable name collide silently |
| `dwarf_map` members | Walked only under `DW_TAG_structure_type` / `DW_TAG_union_type`. Members of a `class` (`DW_TAG_class_type`) are invisible — to the SIL and to the desktop app's signal picker, which enumerates every leaf with no filter |
| `std::array` in DWARF | Its element storage is a member named `_M_elems`, so a traced path becomes `x.duty._M_elems[0]` |
| `DW_AT_specification` on variables | GCC splits any extern-declared-then-defined global (C too, at `-Og`, DWARF 5) into a nameless definition DIE linked to its declaration; the reader used to drop these — 30 firmware statics (`uwTick`, `SystemCoreClock`, the nanopb descriptors) were invisible to the picker and the SIL until the C++ reader work |
| MinGW GCC 15.2 LTO with a mixed-language link | Internal compiler error (`choose_baseaddr`, `i386.cc:7447`) in the LTRANS job when `g++` links C and C++ LTO bytecode together — it crashes inside an unrelated C unit. Workaround in `native.cmake`: `-flto` on C units only, so C++ objects link plain into the otherwise-LTO SIL image. Revisit on a GCC upgrade |

Startup and link infrastructure is already C++-ready: `startup_stm32g431vbtx.s`
calls `__libc_init_array` before `main`, and the linker script carries
`.preinit_array`, `.init_array`, `.fini_array`, `.ARM.exidx`, `.ARM.extab`.
Both toolchain files already `set()` a C++ compiler (inert until a `project()`
enables the language) and apply flags through `add_compile_options`, so the
MCU flags and warning set extend to C++ with no route to a float-ABI mismatch.
CI needs no change: every runner already has a `g++`, including the
`arm-none-eabi-gcc-action` toolchain.

## Step 0 — state the host reads by DWARF path is an interface

Two consumers read firmware statics by **literal DWARF member path**:

- The SIL harness (`read_cvar`), e.g.
  `IO_bridge_data.channels[0].{current_amps,updateCount,sampleTime_us}[N]`
  (`injected_seam.rs`) and
  `app_motorControl_data.channels[0].{isAligned, modeCurrent, faultLatched,
  velocitySetpointCurrent_radPerSec, alignmentOffset_rad, encoderFaultCount,
  phaseCurrent_a[N], busCurrent}` (`north_star.rs`, `common/mod.rs`,
  `board.rs`).
- The desktop app: the signal picker lists every leaf the ELF exposes, and
  saved layouts and watch lists store those paths.

A lookup that no longer resolves fails at runtime, not at compile time.

**Program of record: `dwarf_map` learns C++ before the first idiomatic
pass.** Class members are exactly what the host must see once the control
and estimation modules are classes, so the reader (one crate, shared by the
SIL and the desktop app) grows:

- members under `DW_TAG_class_type`, private ones included, walked like
  `structure_type` today;
- static data members: the defining `DW_TAG_variable` carries only a
  `DW_AT_specification` back to the member declaration, so variables follow
  that link for their name the way functions already follow
  `DW_AT_abstract_origin`;
- base-class members (`DW_TAG_inheritance`) flattened into the derived
  object's path;
- `std::array` flattened so a path reads `x.duty[0]`, not
  `x.duty._M_elems[0]`, and the leaf enumeration lists it as an array.

It ships with a C++ fixture compiled by the native toolchain in the crate's
tests, and lands before Phase D (order of work, step 3). Until then the
mechanical passes change no shapes, so nothing waits on it.

Rules that hold regardless:

- The traced state object stays at namespace scope under its current name;
  variables resolve by bare name, so two namespaces must not reuse one.
- Renaming a traced field or object, or moving it into a class, is a
  deliberate interface change: update the SIL paths and add a layout
  migration in the same commit.

## The module template

Every converted or new module has the same shape, so C consumers, the
tests, and the host tooling keep working:

| File | Role |
|---|---|
| `X.hpp` | The C++ API: `namespace`, types, the class or free functions other C++ modules call |
| `X.h` | The C facade: today's `X_function` names as `extern "C"` declarations, thin wrappers over the C++ API. Kept while any C consumer remains (`main.c`, channel configs, C tests, Unity mocks) |
| `X.cpp` | Implementation. The traced state object stays at namespace scope under its current name |
| `test/test_X.cpp` | Unity, unchanged in structure; mock seams for HW/IO functions are `extern "C"` definitions |

Project-side files (`sw/fw/src/**/*_channels.c`, `*_config.c`) stay C: they
are tables, and the facade header is what they include.

Naming (ruling proposed with the first conversion, in a lean sibling
`docs/cpp-coding-conventions.md`): the facade keeps `IO_bridge_setPhaseDuty`
style; inside, `namespace io::bridge`, PascalCase types without the `_S` /
`_E` suffixes, lowerCamel members, `private:` instead of the `_private_`
infix, `enum class` for enumerations, `constexpr` for constants that were
macros. Single return, `const` locals, explicit parens carry over unchanged.

## Phase A — enable CXX with zero `.cpp` files

Proves the build-system change in isolation. Done once, for the whole tree.

1. `sw/fw/CMakeLists.txt` → `project(pcs_bldc_fw LANGUAGES C CXX ASM)`;
   `sw/lib/c/CMakeLists.txt` → `project(pcs_lib_c LANGUAGES C CXX ASM)`.
2. Beside the C standard block in both: `CMAKE_CXX_STANDARD 20`,
   `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`. C++20, not
   23 — GCC 14 implements 20 completely.
3. In both toolchain files, C++-only flags through a generator expression so
   they cannot leak onto C units:
   ```cmake
   add_compile_options(
     $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>
     $<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>)
   ```
   `-fno-threadsafe-statics` drops the `__cxa_guard_*` calls around
   function-local statics; `-fno-use-cxa-atexit` stops registering static
   destructors that never run here; the unwind flags keep `.ARM.exidx` from
   growing for code that cannot throw.
4. `lib_utils.h`: a `__cplusplus` branch for `COUNTOF` (a `constexpr`
   template over `T (&)[N]`, which rejects pointers the same way).

Gate: all three builds pass; the ARM image is still 94,304 B. If flash moved,
a flag leaked onto the C build.

## Phase B — `extern "C"` the boundary headers, still all C

A `.cpp` unit including `HW_TIM.h` or `HW_ADC.h` emits **mangled references**
that will not resolve against the C-compiled archives, so the guard

```c
#ifdef __cplusplus
extern "C" {
#endif
...
#ifdef __cplusplus
}
#endif
```

goes on every header a C++ unit will include and every facade a C unit
includes. For the first two conversions:

| Header | Reason |
|---|---|
| `io/bridge/IO_bridge.h` | facade: `main.c`, `IO_bridge_channels.c`, `app_motorControl.h`, `test_IO_bridge.c` |
| `hw/TIM/HW_TIM.h`, `hw/ADC/HW_ADC.h` | included by `IO_bridge.cpp` (the `HW_ADC_callback_F` typedef included) |
| `test/mocks/hw/TIM/*.h`, `test/mocks/hw/ADC/*.h` | the shadow headers both unit harnesses compile the driver against |
| `app/motorControl/app_motorControl.h` | facade: `main.c`, `app_motorControl_channels.c`, `app_server_config.c`, `app_userControls.{c,h}`, `app_rgbLedRing.h`, three test/mock files |
| `lib/timer/lib_timer.h`, `lib/filterIIR/lib_filterIIR.h`, `lib/utils/lib_utils.h`, `dev/gateDriver/*.h`, `io/AS5048/*.h` | included by `app_motorControl.cpp` — derive the exact list from its includes at the time |

Enum-only headers (`*_channels.h`) need no guard. Gate: still a pure-C build,
all three builds and every Unity harness unchanged.

## Phase C — rename to `.cpp`, behavior identical

Per module. For `IO_bridge` (476 lines today):

1. `git mv IO_bridge.c IO_bridge.cpp`; update **every** CMakeLists that
   names the file: `io/bridge/CMakeLists.txt`, `io/bridge/test/`, and
   `app/motorControl/test/` (that harness compiles the real `IO_bridge`
   beside `app_motorControl`, so it becomes a mixed-language link too).
2. Fix what C++ rejects — the checklist for any module:
   - C99 array designated initializers (`IO_bridge_complementaryPhase`,
     `IO_bridge.c:41-43`) → positional.
   - Implicit int-to-enum and `void *`-to-typed conversions → explicit casts
     (the existing `(HW_TIM_peripheral_E)0` casts already comply).
   - Designated struct initializers must follow declaration order.
   - `COUNTOF` per Phase A; `_Static_assert` → `static_assert`.
   - Compound literals are a GCC extension in C++; rewrite as named
     temporaries.
   - `volatile` compound assignments (`v++`, `v += x` on a volatile) are
     deprecated in C++20 and warn; split into load/store. `IO_bridge` has
     only volatile pointer stores today.
   - Callbacks registered with C drivers get `extern "C"` linkage; it also
     documents that they run in ISR context.
3. Leave the `[impl->]` tags where they are. Mark `X_data` and its `data`
   alias `constinit` so the compiler proves constant initialization.

Measured on the first rename (2026-09-28): both toolchains compile
`IO_bridge.cpp` unchanged with no errors, only the three `-Wpedantic`
warnings for the array designators; native ctest, debug SIL, and release
SIL (with the LTO workaround above) pass; the ARM image is byte-identical.

Commit with **structure, names, and behavior identical** — the only diff is
language mechanics. The oracle is the module's Unity suite plus the SIL
tests that read its state (`injected_seam.rs` for the bridge; `north_star.rs`
and the fault suites for motor control). Expect a small flash delta from the
`g++` link; record it in the commit.

For `app_motorControl` (438 lines) the same steps apply; its facade has
ten includers (table above), and the mock `mock_app_motorControl.c` used by
the server tests keeps its C definitions behind the guarded facade.

## Phase D — the actual C++, per module, separate commit

Only now introduce idioms: a `Bridge` type over what
`IO_bridge_channelData_S` holds, `constexpr` for the complementary-phase
table, `enum class Phase`, a scoped critical-section guard where the module
pairs the FreeRTOS calls. Step 0 governs the traced state: the object keeps
its name, and any renamed or relocated field ships with the SIL paths and a
layout migration.

Separating the language change from the design change is both good practice
and better pedagogy: the Phase D diff shows what each idiom cost or saved in
flash.

## Order of work

Each step is one PR, held at CI green for review:

1. Phase A + B for the tree, Phase C for `IO_bridge`.
2. Phase C for `app_motorControl` (its harness and the server mocks come
   along).
3. `dwarf_map` C++ support (Step 0), with its fixture and tests; the SIL
   and the desktop app pick it up as the shared crate.
4. Phase D for both modules, plus the new C++ transforms library
   (`fw~mc_013`) and the modulator at the bridge boundary — a
   `setVoltageVector` entry that reads the bus voltage and applies
   `fw~mc_014` (spec work: a bridge spec for the entry, via `pcs_spec`).
5. The V/f method in C++ `app_motorControl` (`fw~mc_010`, `fw~mc_015`,
   `fw~mc_016`, `fw~mc_017`), SIL coverage, bench spin at reduced voltage.
6. From here new modules are C++: `app_rotorEstimation`, the FOC current
   loop.

`main.c` stays C: it is board glue and task creation, and every module it
calls has a facade. It converts only when it has a reason to.

## Verification gate

After every phase:

```bash
tools/build_native.sh          # includes ctest
tools/build_arm.sh             # --print-memory-usage against 94,304 B / 28,920 B
tools/run_sil.sh               # and tools/run_sil.sh --debug (CI's other flavor)
tools/oft/oft.sh trace specs/ sw/ README.md   # expect 1130 total, 34 defects
```

## Open questions

- Whether test files convert to `.cpp` with their module (they can use the
  C++ types directly) or stay C against the facade. Proposal: convert with
  the module.
- Whether the `-O2` link-path list in `sw/lib/c/CMakeLists.txt` (which names
  `io_bridge`) should stay as is; its options apply to the C++ unit
  unchanged, which is the intent.
