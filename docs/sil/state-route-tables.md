# State Table + Route Table (core framework data structures)

The two fundamental data structures of the SIL framework. Together they
replace any sim-specific getter/setter API in the firmware (see
[`ffi-boundary.md`](ffi-boundary.md)): the framework observes and drives the
firmware purely by reading/writing its native memory, and wires everything
together declaratively.

---

## 1. State Table

A single flat namespace of **every piece of state in the system**, each as an
addressable, typed **entry**. It is the flat, **member-agnostic backbone** and the
**sole signal registry**: the framework may read or write any entry on any tick,
and any [member](architecture.md#member-model) may register any entry at any time.

All values flow through one common **`Value`** enum (scalars + `Bytes`/`Record`
for structured comms payloads), which keeps a *heterogeneous* registry uniform.

**`sig_type` names a *backing regime*** — where the entry's authoritative value
lives, not what kind of member owns it:

| `sig_type` | Backing regime | Typical source | Accessed via |
|---|---|---|---|
| `cvar` | table **mirror** of authoritative memory inside a firmware instance | every C `static` (+ nested members, array elements) | DWARF map + ASLR slide → live in-process address ([`ffi-boundary.md`](ffi-boundary.md) §4); sampled into the table each firmware tick |
| `vsig` | **framework-resident** — the entry's value *is* the authority (no external mirror); it abides entirely within the framework | a model/peer member's state (inputs, outputs, internals) | pushed into the table by the member each advance |
| `usb_cdc` / `spi` / `i2c` / `uart` | **comms** — a framework-resident transport payload (a future backing regime) | logical packet payloads on a serial bus | a framework queue, fed by a sim-HW-driver C→Rust upcall (*Comms entries* below) |

**Open registration — no inference of member type from `sig_type`.** Registration
is a runtime act any member performs directly on the table; it is never
prescriptive per member type. Any member may register a signal of *any* `sig_type`
at any time (a model may register a `cvar`, a firmware member a `vsig`), and
**nothing infers a member's kind from the `sig_type` it registers**. A `cvar` says
"this entry mirrors firmware memory," not "a firmware member owns it."

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
- **Non-`cvar` entries are registered by members** (Rust-side) as a runtime act on
  the table: a model member registers its `vsig`s (in `set_enabled(true)` / on
  advance), sim-HW bus drivers their comms channels. `cvar` mirrors are the one
  auto-derived case (DWARF). Re-registration of an identical entry is an idempotent
  no-op, so a firmware member re-registering its ports across a reboot preserves the
  entry's history.
- **Any entry is readable and writable at any tick.** There is *no* clobber
  protection: if a route drives a `cvar` and a test also writes it, the route
  wins on its next tick — by design. Targeted injection is done by **suspending
  the relevant route** (below), an explicit control, not by warnings.

### Naming convention

Every entry has a canonical key: **`<sig_type>:<source>:<local>[:<modifier>]`**.
`:` is the delimiter (C paths use `.`/`[]`, never `:`).

- **`sig_type`** — `cvar`, `vsig`, or a bus (`usb_cdc`/`spi`/`i2c`/`uart`).
- **`source`** — the producer namespace: the firmware instance for `cvar`
  (`pcs_bldc`, future-proofs the multi-device stretch goal), the model for
  `vsig` (`motor`), the logical channel/peer for comms (`encoder`).
- **`local`** — the local name: a DWARF path for `cvar`
  (`HW_ADC_data.channelData[0].counts[6]`), a model signal for `vsig`
  (`phase_u_voltage`), a packet/stream for comms.
- **`modifier`** (optional) — direction (`tx`/`rx`, `in`/`out`) or a derived
  view (`decoded`). Units/role/description live as entry metadata, not in the
  key.

Examples: `cvar:pcs_bldc:HW_ADC_data.channelData[0].counts[6]`,
`vsig:motor:phase_u_voltage`, `spi:encoder:rx:decoded`.

### Comms entries (the framework is the wire)

Because the firmware runs in virtual space, the framework *is* the transport
for every serial bus. The sim-HW bus driver (e.g. sim `HW_USB`, sim `HW_SPI`)
calls a **C→Rust upcall** with each payload; the framework (1) records it in the
comms entry's history and (2) routes it to the destination (a peer model now, an
external transport — the real desktop app, D5 — later).

- **Logical payloads, not raw bytes.** We own the sim-HW driver code, so a comms
  entry holds the *logical contents* of the packet (a `Value::Record`/`Bytes`
  shaped by the transport), not a bitstream — far nicer to assert on and route.
- **Timing.** A transaction's completion schedules a **one-shot interrupt** (D8)
  quantized to the base `dt`; rx is delivered then.
- **History is uniform.** A comms entry is a historian like any other — just a
  timeseries of `Value`s (here, packets) rather than scalars (D12).
- Comms is **designed-in now, built after** the model + interrupt (D8) layers,
  and external-transport routing after that (D5).

## 2. Route Table

A list of **routes**. A route is `source → destination`, both State Table
entries. **Every tick the framework copies source → destination.** That's the
entire data plane — no bespoke firmware code moves data.

Design rules:

- **Routes are pure typed transport.** A route copies a value; it does not
  transform it. Endpoints must be type-compatible (trivial width coercion
  allowed; see open questions).
- **Propagation is table-mediated — routes never touch a backend.** A route is a
  pure State Table operation: it reads a source *entry* and records a destination
  *entry*, and that is all. Moving a value between two entries is always a
  table-only act, because a `cvar` entry is the table's *mirror* of firmware
  memory and a `vsig` entry *abides* in the framework. **Members sync their own
  mirrors** on their own clock: a `FirmwareMember` **flushes** its *driven* `cvar`
  entries into firmware memory (and **samples** its *sampled* ones back out) around
  its firmware tick; a model reads a routed `vsig` input straight from the entry.
  The route is indifferent to which — no bespoke firmware code moves data.
- **A destination is any registered signal of any `sig_type`.** A `cvar`
  destination (model output → a firmware sensor input, flushed by the consuming
  firmware member) and a `vsig` destination (a model input, read by the consuming
  model) work through the identical path — a `record` into the entry — with no
  per-`sig_type` restriction and no new seam. The table is a flat, member-agnostic
  registry.
- **Override pins a destination against its route.** Because a route drives its
  destination via `record`, `set_override` on that entry makes the record a no-op —
  the route cannot drive a pinned destination. Fault injection composes with
  routing at **zero extra mechanism** (it is the same pin the historian uses).
- **Conversions are models, not routes.** A motor model emits *amps*; a
  firmware ADC static holds *counts*. The amps→counts conversion lives in a
  **sensor model** whose input and output are themselves State Table entries:
  `model.motor.i_a → model.sense_a.in`, then `model.sense_a.out →
  fw::adc_counts_a`. Routes stay dumb pipes; all computation lives in models.
  This keeps the Route Table a readable wiring diagram.
- **Add / remove at runtime; validity is the designer's job.** `add` is
  permissive — a route may be authored before its endpoints are registered, and
  `remove` may drop one at any time (`suspend`/`resume` toggle without removing).
  The framework does not *prevent* invalid wiring; it **fails loudly** at
  `propagate` instead: an unregistered source *or* destination (symmetric) is a
  wiring bug and errors. A registered-but-never-recorded source has nothing to copy
  and is silently skipped that tick.
- Authoring is one flat list; the framework derives execution order (below).
- **Routes are first-class and suspendable.** Each route has an `enabled` flag;
  a control API (`suspend`/`resume`, addressable by source/destination) lets a
  test cut a route to inject a value at its destination directly — the core
  **fault-injection** primitive (driven from the Python layer). A suspended
  route simply isn't propagated that tick.

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

- **Mutual exclusion holds.** Propagation (step 2) is a pure State Table
  operation — it records between *entries* and never touches firmware memory. The
  firmware member does the memory sync (flush driven `cvar` entries in, sample
  outputs back out) inside its own advance while the firmware is quiescent
  ([`ffi-boundary.md`](ffi-boundary.md) §5), so reading firmware-*output* statics
  and writing firmware-*input* statics is race-free. Reading an output here simply
  reads the previous tick's value — exactly the one-step sampling delay we want.
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

> **OPEN — route-hop latency is NOT settled.** The "one tick per hop" delay
> described above is *not* a decided contract; it is under an owner design review.
> The firm requirement is that routing add **no artificially induced per-hop
> latency**. The current engine implements an **interim** placement — it propagates
> routes *before each member* in registration order, so along that order a value
> reaches its consumer with zero added latency (a model output routed to a firmware
> `cvar` lands before that firmware advances in the same step). Treat any per-hop
> latency figure here as provisional pending that review.

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
- ~~**Model registration API** (Rust): how models declare their entries.~~
  **Resolved:** open registration — a member registers its entries directly on the
  table at runtime (`StateTable::register`, idempotent), inside
  `Member::set_enabled` / `advance`. No `signals()` declaration.
