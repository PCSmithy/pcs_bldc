# C coding conventions

This is the source of truth for how C code in this project is written.
Applies to all firmware code under `sw/lib/c/shared/`, `sw/fw/src/`,
and any future C code anywhere in the tree. Every new module follows
these conventions; deviations require a justifying comment.

The conventions are **MISRA-flavored** — drawn from MISRA C:2012
Advisory rules where they pay back at this project's scale. We don't
claim strict MISRA compliance (no static-analysis enforcement yet),
but we follow the spirit: predictable structure makes the code
maintainable and reads as polished to anyone landing on it cold.

The canonical worked examples for every rule below are
`sw/lib/c/shared/hw/ADC/` (multi-channel module) and
`sw/lib/c/shared/hw/systemClock/` (single-instance module). Copy from
those when in doubt.

## 1. Naming

### Functions

- **Public:** `<Module>_<camelCaseName>`
- **Private (static):** `<Module>_private_<camelCaseName>`

The `_private_` infix on static functions is mandatory. Never use bare
`snake_case` for any function name.

```c
// Public
bool HW_ADC_init(...);
bool HW_ADC_getCount(...);
void HW_systemClock_init(...);

// Private
static uint8_t HW_ADC_private_resolutionToNumBits(...);
static bool    HW_ADC_private_initOneChannel(...);
```

### Variables and struct fields

`camelCase`. Examples: `numEnabledInputs`, `rankOrder`, `numBits`,
`channelData`, `tickCounter`, `channelConfig`, `inputIndex`.

Single-letter loop counters are fine: `ch`, `r`, `input`, `i`.

Module-private file-scope variables: `<Module>_<descriptiveName>` —
e.g. `static HW_ADC_data_S HW_ADC_data;`.

Module-private file-scope `const` pointer aliases: lowercase
descriptive name — e.g.
`static HW_ADC_data_S * const data = &HW_ADC_data;`.

### Types

- **Structs:** `_S` suffix → `HW_ADC_channelConfig_S`,
  `HW_ADC_data_S`, `HW_systemClock_config_S`.
- **Enums:** `_E` suffix → `HW_ADC_channels_E`,
  `HW_ADC_triggerMode_E`.
- Module prefix on every typedef.

### Macros and defines

`UPPER_SNAKE_CASE` with module prefix:
`HW_ADC_INPUTS_PER_CHANNEL`, `HW_ADC_POLL_TIMEOUT_MS`,
`BUILD_TARGET_STM32G4`.

### Enum values

`UPPER_SNAKE_CASE` with the enum's name as prefix (drop the `_E`):
- `HW_ADC_TRIGGER_SOFTWARE`, `HW_ADC_TRIGGER_TIMER`
- `HW_ADC_XFER_POLLED`, `HW_ADC_XFER_DMA`
- `HW_ADC_CHANNEL_1`, `HW_ADC_CHANNEL_COUNT`

## 2. Code patterns

### Single return per function (MISRA Rule 15.5)

Every function — public, private, void, bool, all of them — has
**exactly one `return` statement**, at the end. No early returns.

**Bool-returning functions:** accumulate via a `bool ret = <default>;`
local, set the success path inside an `if (...)` block, return `ret`
at the end:

```c
bool HW_X_doThing(...)
{
    bool ret = false;
    if ((cond1) &&
        (cond2) &&
        (cond3))
    {
        // do work
        ret = true;
    }
    return ret;
}
```

For sequential init paths with multiple failure points, use
`bool ret = true;` and gate each subsequent step with
`if (ret) { ... if (failure) { ret = false; } }`. Verbose but
single-return.

**Void functions:** wrap the body in `if (cond) { body }` instead of
early-returning:

```c
// WRONG (early return)
void HW_X_run(void)
{
    if (!data->initialized) return;
    // ...
}

// RIGHT (body wrapped)
void HW_X_run(void)
{
    if (data->initialized)
    {
        // ...
    }
}
```

`break` and `continue` inside loops are fine — the function still has
one return.

### Explicit parens on compound boolean logic

Never rely on operator precedence in compound `&&` / `||`
expressions. Every operand gets its own parens, even simple
identifiers:

```c
// WRONG
if (config == NULL || config->channels == NULL) ...
if (rank < 1U || rank > MAX) ...
if (a && b) ...

// RIGHT
if ((config == NULL) || (config->channels == NULL)) ...
if ((rank < 1U) || (rank > MAX)) ...
if ((a) && (b)) ...
```

Multi-line formatting for readability when the chain gets long:

```c
if ((out != NULL) &&
    (data->initialized) &&
    (channel < HW_ADC_CHANNEL_COUNT) &&
    (inputIndex < HW_ADC_INPUTS_PER_CHANNEL) &&
    (data->config->channels[channel].inputs[inputIndex].enabled))
{
    ...
}
```

Ternary conditions get the same treatment:
`((numEnabled > 1U)) ? A : B`.

### Comments explain non-obvious context only

A comment exists for exactly one reason: to explain something the code
cannot say for itself — a *why*, a constraint, a non-obvious gotcha. If
a comment restates what the code already shows, delete it.

Three hard rules:

- **Only non-obvious context.** Comment the reason, not the mechanics.
  `i++` does not need `// increment i`; a `& (x - 1)` power-of-two check
  does benefit from a note on what it rejects.
- **Never reference past implementations.** No "was X, now Y", no
  "previously", no "changed from", no commented-out old code. Source
  control is the record of history; a comment describes the code as it
  is now, for someone reading it cold.
- **1–2 lines, hard limit.** If the explanation needs more, the code is
  too complex — split or rename until a line or two suffices.

```c
// WRONG — restates the code / references history
// loop over the buses (used to be channels before the bus/channel split)
for (HW_SPI_bus_E bus = 0U; bus < HW_SPI_BUS_COUNT; bus++)

// RIGHT — explains the non-obvious why, in one line
// Validate every channel before touching hardware so a bad config
// fails init cleanly rather than half-configuring the peripheral.
for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
```

### const where possible (MISRA Rule 8.13)

Every variable that is not reassigned after initialization is declared
`const`:

- Local single-assignment values: `const uint32_t maxCounts = ...;`
- Pointer aliases that aren't reassigned:
  `const T * const p = &x;` (the `* const` makes the pointer itself
  `const`, distinct from `const T *` which makes only the pointee
  `const`).
- Function parameters that are pointer-typed and never reassigned:
  `bool foo(const T * const arg)`.

**By-value parameters are NOT const-qualified.** They're local copies;
`const` on them is noise:

```c
// RIGHT — pointer param const-qualified, by-value params left alone
bool HW_ADC_getCount(HW_ADC_channels_E channel,
                     uint8_t inputIndex,
                     uint32_t * const out);
```

**Things typically not const:** loop counters mutated by `i++`/`i--`,
accumulators (`bool ret`, `uint8_t count`), variables written via
out-pointer (`uint32_t counts; getX(&counts);`).

## 3. Module conventions

### Init functions return bool; only main.c calls Error_Handler

Every `HW_*_init` / `IO_*_init` / similar init function returns
`bool` — `true` on success, `false` on any failure path. Library code
**never** calls `Error_Handler` directly.

`main.c` is the single place that handles failure:

```c
bool initSuccess = true;
initSuccess &= HW_systemClock_init(&HW_systemClock_config);
initSuccess &= HW_ADC_init(&HW_ADC_config);
// ... more inits
if (!initSuccess)
{
    Error_Handler();
}
```

Sim implementations that can't actually fail still return `bool`
(always `true`) so the function signature is uniform across targets.

`Error_Handler` itself lives in `sw/fw/src/main.c`, so it's always
part of the executable's link — independent of which optional library
modules end up linked in.

### Module file-naming: `_channels` (multi-instance) vs `_config` (single-instance)

- **Multi-channel modules** (the typical case — ADC, SPI, UART, GPIO,
  PWM, timers): the project-side extension files are
  `HW_<Module>_channels.{h,c}` and the CMake INTERFACE library that
  carries the include path is `pcs_<Module>_channels`.
- **Single-instance modules** (rare — system clock, possibly the
  watchdog, RTC): files are `HW_<Module>_config.{h,c}` and the
  interface lib is `pcs_<Module>_config`.

A "channel" implies one of N independent peripheral instances. When
there's only one (system clock), calling it a "channel" misleads —
it's just the configuration.

The library-side header (under
`sw/lib/c/shared/hw/<Module>/<target>/`) `#include`s the consumer's
extension header by the matching name —
`#include "HW_<Module>_channels.h"` or `#include "HW_<Module>_config.h"`.

### Where the channels enum lives: library-side vs project-side

The project-side `_channels.h` extension header (and matching
`pcs_<Module>_channels` INTERFACE lib) only exists when there's
something project-specific to declare. The deciding question is whether
the channels enum is **HW-fixed across the entire MCU family** or
**varies per chip variant**:

- **Library-side** (in `HW_<Module>.h`): when the enum is HW-fixed for
  the whole family. Example: `HW_GPIO_port_E` — every STM32G4 variant
  has GPIOA..GPIOG. No reason to make each project re-declare it.
- **Project-side** (in `HW_<Module>_channels.h`): when the enum varies
  per chip variant. Example: `HW_ADC_channels_E` — STM32G431/G441 have
  2 ADCs, G473/G474/G483/G484 have 5 ADCs, so the enum body genuinely
  differs across projects targeting different G4 variants.

When the enum is library-side, the project-side `_channels.h` and
`pcs_<Module>_channels` INTERFACE lib aren't needed and shouldn't be
created — leaving empty placeholder files counts as ceremony. The
`HW_<Module>_channels.c` file (config instance) stays regardless;
that's always project-specific.

Canonical examples in tree: `HW_GPIO` (library-side enum, no
`channels.h`) vs `HW_ADC` (project-side enum in `HW_ADC_channels.h`).

## See also

- The **channelization pattern** itself (where files live, how library
  and project halves split, CMake target conventions) is documented in
  [CLAUDE.md](../CLAUDE.md#channelization-pattern-canonical-idiom).
- For the build-system context (toolchains, dual-target, library
  layout), the **Build System** section of CLAUDE.md.
