# State Table + Route Table (core framework data structures)

The two fundamental data structures of the SIL framework. Together they
replace any sim-specific getter/setter API in the firmware (see
[`ffi-boundary.md`](ffi-boundary.md)): the framework observes and drives the
firmware purely by reading/writing its native memory, and wires everything
together declaratively.

---

## 1. State Table

A single flat namespace of **every piece of state in the system**, each as an
addressable, typed **entry**. The framework may read or write any entry on any
tick.

Two backing kinds, one uniform interface (`read(buf)` / `write(buf)` + type):

| Backing | Source | Located via |
|---|---|---|
| **Firmware** | every C `static` (incl. function-local statics) and its nested members | DWARF map + ASLR slide → live in-process address ([`ffi-boundary.md`](ffi-boundary.md) §4) |
| **Model** | plant-model state fields (motor, encoder, sensors, power) | Rust-side storage |

Notes:

- **Firmware entries are auto-derived** by walking DWARF at load — no manual
  registration, no firmware annotation. The firmware portion of the State
  Table *is* the DWARF symbol/type map, made first-class.
- **Granularity is leaf-level.** Structs/arrays flatten to scalar leaves via
  DWARF offsets, so a route can target one field (e.g.
  `HW_ADC_data.channelData[0].counts[3]`). Aggregates remain addressable too.
- **The full namespace is cheap.** Entries are metadata (name → addr/type);
  values are read on demand. Holding an entry per static costs nothing until
  something routes or reads it.
- **Entries are also historians.** Beyond the current value, each entry
  accumulates a **change-logged, timestamped time series** (discretes on exact
  change, floats on an ε-deadband) — **the whole namespace is traced by
  default** (stacks/large arrays excluded). The dumped table is the full
  timeseries history that Python SIL tests evaluate against. See
  [`signal-trace.md`](signal-trace.md) (D12).
- **Model entries are registered** by the models (the only "registration" step
  — and it's Rust-side, not firmware).
- Entries can be marked RO/RW; firmware-output signals are typically RO from
  the test's perspective, model inputs RW, etc. (advisory, for safety checks).

Keying: a stable path string per entry — firmware symbol paths from DWARF
(`module::var`, member/array suffixes), model paths (`model.motor.i_a`).

## 2. Route Table

A list of **routes**. A route is `source → destination`, both State Table
entries. **Every tick the framework copies source → destination.** That's the
entire data plane — no bespoke firmware code moves data.

Design rules:

- **Routes are pure typed transport.** A route copies a value; it does not
  transform it. Endpoints must be type-compatible (trivial width coercion
  allowed; see open questions).
- **Conversions are models, not routes.** A motor model emits *amps*; a
  firmware ADC static holds *counts*. The amps→counts conversion lives in a
  **sensor model** whose input and output are themselves State Table entries:
  `model.motor.i_a → model.sense_a.in`, then `model.sense_a.out →
  fw::adc_counts_a`. Routes stay dumb pipes; all computation lives in models.
  This keeps the Route Table a readable wiring diagram.
- Authoring is one flat list; the framework derives execution order (below).

## 3. Tick evaluation order

A single fixed period, every tick — **no per-route phase inference**:

```
per tick:
  1. advance models (dt)         → model output entries updated
  2. propagate routes            snapshot ALL sources, then write ALL dests
  3. sil_fw_advance_tick()       firmware runs to quiescence (D1)
  4. record signals; asserts/injection
  5. pace (realtime: sleep · fast: now)
```

Why one uniform propagation pass is correct:

- **Mutual exclusion holds.** Propagation (step 2) runs while the firmware is
  quiescent ([`ffi-boundary.md`](ffi-boundary.md) §5), so reading firmware-
  *output* statics and writing firmware-*input* statics in the **same** pass is
  race-free. Reading an output here simply reads the previous tick's value —
  which is exactly the one-step sampling delay we want.
- **Order-independent.** The pass **snapshots every source first, then writes
  every destination**, so route order in the table never changes the result,
  and a chain `x→y→z` advances one deterministic hop per tick (one delta
  cycle). This is the rule that matters for determinism.

**Consequence — a small, fixed, consistent latency.** A value leaving the
firmware reaches the models the next tick; each hop adds one tick. At the base
`dt` (fastest ISR rate, D6) this is sub-sampling-period and physically faithful
— real sensors/actuators sample with delay, and zero-delay algebraic loops are
ill-posed in discrete time anyway.

If a deep model→model chain ever accumulates too much latency, the lever is
ordering the **model updates** (step 1) in dataflow order — *not* adding route
phases. Routes stay uniform.

## 4. What this subsumes

The earlier draft split firmware interaction into a "data plane" (explicit
sim getters/setters) and an "observability plane" (DWARF). Those collapse into
one mechanism over the State Table:

- **Standing routes** = the data plane (sensors in, actuators out, every tick).
- **Ad-hoc read/write of any entry** = observability + injection (assertions,
  plotting, fault injection) — same table, just imperative instead of standing.

One namespace, one access path, zero firmware-side data code.

## 5. Why this shape

- **No firmware coupling.** Nothing in the firmware knows about the sim; no
  getter/setter to write or keep in sync as the firmware grows.
- **Total observability.** Any static is reachable for assertions/plots,
  including deep estimator/FSM internals, with no instrumentation.
- **Declarative wiring.** The Route Table is the system's signal-flow diagram;
  scenarios reconfigure it (swap a model, inject a fault) without touching
  firmware or C.
- **Uniform across realtime/fast.** Routes and entries are framework-side and
  deterministic; only pacing differs between modes (D1).

## 6. Open questions

- **Route transforms:** strictly pure copy + width coercion, or allow a small
  declarative scale/offset on a route for convenience? (Leaning pure; push all
  real conversion into models.) Ties to D6 (model↔firmware representation).
- **Model-update ordering** (step 1): declaration order vs dataflow/topological
  order, if deep model→model chains ever make per-hop latency matter. (Route
  propagation itself is already order-independent via snapshot-then-write.)
- **Auto-registration scope:** register *all* DWARF statics (incl. FreeRTOS/HAL
  internals) or filter to a project allowlist for a tidier namespace? (Full
  namespace is cheap; filtering is cosmetic.)
- **Entry keying / path syntax** for nested members and array elements.
- **Model registration API** (Rust): how models declare their entries.
