# C++ coding conventions

Companion to [`c-coding-conventions.md`](c-coding-conventions.md), which
still rules everything it covers: single return, explicit parens, `const`
locals, lean comments, init functions returning `bool`. This file holds
only what C++ adds, as ruled by the conversions so far (`IO_bridge`,
`app_motorControl`). It grows when a new choice is made, not before.

## The transition

New control code is C++; an existing module converts when it is next
touched for a real change, never as an exercise. `main.c` is board glue
and converts only when it has a reason to. A converted module keeps its
C facade while any C consumer remains.

## Language subset

C++20. No exceptions, RTTI, heap, `<iostream>`, `std::function`,
`std::string`; `std::array::at()` is out, use `operator[]`. The toolchain
files enforce the flags.

## Module shape

| File | Role |
|---|---|
| `X.h` | The C facade, `extern "C"` guarded: the `X_function` names every consumer calls today. Project tables (`X_channels.c`) and Unity tests include it |
| `X.cpp` | The class and the facade's definitions |
| `X.hpp` | Appears only when another C++ module needs the class directly. Neither converted module has one yet |

A module is not split between languages on purpose: when it converts, its
project-side table converts with it if there is a reason to.

## The class per channel

- One class per channel instance (`Bridge`, `Motor`), held in a
  `std::array` inside the module's traced data struct, which stays at
  namespace scope under its existing name, marked `constinit`.
- `public:` is the operations; `private:` is every data member and
  helper. The `_private_` infix is replaced by `private:`; static free
  helpers that take no `this` keep `X_private_name`.
- Every public member checks its own preconditions, including "am I
  initialized" (`config != NULL`). The facade owns exactly one check, the
  channel index it uses to reach the object, and forwards.
- `init` validates its channel's config and commits state only on
  success. `run1ms` is walked over every slot; an uninitialized object
  declines on its own.
- Members are written `this->member`, so a member read is visible as one.
- A C callback registered with a driver is a `static` member trampoline
  taking `this` as the context pointer.

## Names

PascalCase types with no `_S` / `_E` suffix; lowerCamel members and
methods; constants that were macros stay macros until a `constexpr`
earns its place.

## Scoped enums

`enum class` does not index an array without a cast. A type used as an
index with a `COUNT` sentinel (`IO_bridge_phase_E`) stays an unscoped enum
until the arrays it indexes become a struct indexed by it; the mode enum
also waits on its C consumers. A state or mode enum in a module whose
consumers are all C++ is the first `enum class` candidate.

## Tests and host visibility

Unity tests stay in C against the facade and observe private state only
through its effects. No accessor, `friend`, or include-the-unit exists
for a test. The SIL and the desktop app read private members through
DWARF; a renamed or moved traced field ships with its SIL paths and a
layout migration in the same commit. The traced object stays at namespace
scope: `dwarf_map` resolves variables by bare name, so two namespaces must
not reuse one.

## Gotchas, measured

- C99 array designators (`[PHASE_U] = {...}`) are a `-Wpedantic` warning in
  C++20; write them positionally. Struct designators must follow
  declaration order. Compound literals become named temporaries.
- `volatile v++` / `v += x` are deprecated in C++20; split into load/store.
- `COUNTOF` has a `__cplusplus` template branch; `_Static_assert` is
  `static_assert`.
- MinGW GCC 15.2 ICEs in LTO when `g++` links mixed C/C++ bytecode:
  `native.cmake` applies `-flto` to C units only. Revisit on a GCC upgrade.
- `dwarf_map` flattens `std::array` by its storage member's name, which is
  `_M_elems` under libstdc++ and `__elems_` under libc++ (the macOS SIL);
  its C++ fixture is a checked-in GCC ELF, so a libc++ regression shows
  only on the macOS runner.
- OFT scans `.cpp` / `.hpp` with no config change.
