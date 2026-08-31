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
| `usb_cdc` / `spi` / `i2c` / `uart` | **comms** — a framework-resident transport payload | logical packet payloads on a serial bus | `spi` has landed as **duplex transactions**: an engine-owned `DuplexRouter` couples any initiating member to a linked peer synchronously, and the engine records each exchange as `:tx`/`:rx` event entries (`Value::Bytes`). `usb_cdc`/`i2c`/`uart` join as they land (*Comms entries* below) |

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

### Units (one signal per quantity; conversion at the boundary)

**One physical quantity is ONE signal, stored in ONE canonical unit.** Units
are never part of a signal's identity — there is no `angle_deg` alongside an
`angle_rad`, and no unit segment in the key. A caller who wants a different
unit asks for a *conversion at the table boundary*; the stored history stays
a single canonical series.

- **Canonical unit = the unit declared at `register(id, Some("rad"))`.** The
  declaration is load-bearing: it names the unit every stored sample is in.
  Convention: SI (or the SI-derived natural unit) for dimensioned quantities;
  signals with no meaningful unit (duty in [0,1], raw counts, flags) register
  `None` and get no conversion service — bare access only.
- **The unit ask** rides the string-keyed API as a bracket suffix on the id:
  `write("vsig:as5048_motor:angle[deg]", 90.0)` converts deg→canonical on the
  way in; `read("…:angle[deg]")` converts on the way out. The suffix is
  per-call, parsed off before id resolution — it is never stored and never
  distinguishes entries. A **bare id operates in the canonical unit.**
  A trailing **non-numeric** bracket is the unit ask; an **all-digit** bracket
  (`counts[6]`) is a cvar array index and stays part of the id. Unit names may
  not start with a digit, so the two never collide.
- **Conversion registry:** a runtime table `unit → (dimension, scale, offset)`
  relative to the dimension's base unit; a conversion resolves ask-unit →
  base → canonical and requires both units to share a dimension. Linear
  (scale + offset) only — that covers deg/rad, RPM/rad·s⁻¹, mV/V, °C/K.
  Built-ins ship for the units the sim uses; `add_unit(name, dimension,
  scale, offset)` extends it at runtime.
- **Fail loud, never guess:** unknown unit, dimension mismatch
  (`angle[V]`), a unit ask on a `None`-unit signal, or a conversion against a
  non-float column is an `Err` — no silent pass-through.
- **Boundary-only.** Conversion lives exclusively in the string-keyed
  `write`/`read` layer (tests, scenarios, Python bindings). Routes, the
  mirror sweep, ports, and members always move canonical values natively —
  the native-format principle and the typed hot-path lanes are untouched.
  `cvar`s are whatever unit the firmware chose; the table never infers or
  converts them (they register `None` unless a human declares otherwise).

Worked example: the encoder model registers `vsig:as5048_motor:angle`
(canonical `rad`). A test writes `angle[deg] = 90.0`; the table stores
`1.5708`; the model reads canonical radians; a later
`read("…:angle[deg]")` returns `90.0`. Coherent by construction — there is
nothing to keep in sync.

Non-goals (deliberate): no `uom`/dimensional-analysis dependency (runtime
string-keyed values fight compile-time quantity types; linear conversions
don't need them); no compound-unit algebra; no hot-path conversion. Future
(unbuilt): route validation could check dimension compatibility across a
route's endpoints — the wiring-error class unit metadata exists to catch.

### Firmware "ports" (ordinary Signals, registered from C)

**"Port" is firmware-member vocabulary, not a voyant concept.** Table-side, a
port is a plain, ordinary Signal — indistinguishable from any other entry, with
no special type, flag, or treatment anywhere in the framework. The word only
describes *why the firmware member cares about that particular Signal*: it is
one its C-side sim drivers registered in order to consume state into, or
broadcast state out of, the firmware executable. Nothing outside the firmware
member (and its `backend` internals) should reuse the term or expect a "port
kind" to exist.

Firmware members register these signals via their **sim HW drivers at
runtime** — the C-side counterpart of open registration. The
seam is a hook vtable installed over the control ABI (`sil_fw_setHooks`, called
before `sil_fw_start`), wrapped driver-side by the null-safe helper
`SIL_ports_*` (`sw/lib/c/shared/hw/sim/ports/`): with no hooks installed —
standalone native runs, Unity tests — register returns an invalid handle, read
returns false, write no-ops, and drivers behave exactly as if the seam did not
exist.

- **The C side never knows its instance name.** A driver registers only
  `{sig_type, local, unit}` (e.g. `vsig`/`ADC1_IN6`/`V`); the consuming
  `FirmwareMember` prefixes its own member name to form the entry id
  `{sig_type}:{member}:{local}` — two boards may run the same DLL as distinct
  members.
- **Native format.** Port values flow member-to-member in native units
  (volts→volts). The sim HW driver owns any conversion to its C-memory
  representation (volts→counts per its own numBits/vref) — the conversion lives
  where real hardware does it. Scalar ports carry a `double` (pins are levels);
  bus transactions do not flow through the port cache at all — they use the
  synchronous **DuplexTransfer** primitive (*Comms entries* below).
- **Mirror-sync / cache mediation — no re-entrancy.** C never touches the
  State Table mid-tick. Per firmware tick the member runs **three fixed phases**
  over its signal *bindings* (ports, cvars — each an optional in-half and/or
  out-half): **in-sync → `advance_tick` → out-sync**. In-sync (table → firmware):
  apply pending port registrations then fill **every** port's *input cache* from
  the entry's current value (never driven → `None` → C read returns false →
  driver fallback), and flush the **fresh** cvars in (below). `advance_tick` (C
  reads caches, buffers writes). Out-sync (firmware → table): drain the *output
  buffer* into the table, and **sweep the whole cvar leaf list** back into the
  mirror. A future transport `sig_type` is a new binding with its own halves —
  the phase sequence never changes, and direction stays on the binding mechanism,
  never on a signal.
- **The cvar mirror is automatic and accurate — no per-signal declarations.**
  The firmware member enumerates its traceable leaves once (at enable) and
  registers `cvar:<member>:<leaf>` for each; the out-sync **sweep** records every
  one memory→table each tick (`record_mirror`), so the cvar namespace is a live
  mirror of firmware memory. There is no `drive`/`sample` list — the whole
  namespace is traced by default (the D12 end-state). Epsilon dedup + retention
  keep the historian bounded despite the full sweep. For performance the sweep
  reads through **pre-resolved handles** (address/type cached at enable), never
  re-resolving DWARF per tick.
- **In-sync flush is sparse — only the *fresh* cvars.** The State Table marks a
  **dirty set**: `record` / `force_record` (route, test, model output) mark the id
  dirty; the mirror sweep (`record_mirror`) does **not**. Each tick the member
  flushes its namespace's dirty ids (`take_dirty`, source-scoped), filtered to
  `cvar`. This is sound because the sim is single-threaded: between one tick's
  sweep and the next tick's flush no firmware code runs, so a table entry differs
  from memory **iff** the framework command-wrote it — "flush fresh" ≡ "flush
  all", done cheaply. Writes are one-shot, last-writer-wins: if firmware
  overwrites a value mid-tick, the sweep mirrors that back and the framework does
  not re-assert — a value persists only as long as nothing else writes it.
- **Exclusion policy (built-in).** Enumeration expands nested struct members and
  array elements to scalar leaves, but an array with more than a size threshold
  (**default 32**) is excluded whole — this drops FreeRTOS task stacks, `ucHeap`,
  512-byte scratch buffers, etc. Multi-dimensional and unknown-length arrays, and
  non-data leaves (pointers, functions, opaque aggregates), are skipped too; a
  depth/leaf-count safety cap guards pathological DWARF. A firmware member can
  `skip_cvar_registration_by_prefix(prefix)` a noisy subtree or
  `register_cvar_in_state_table(path)` a specific over-threshold leaf it needs to
  drive (e.g. one byte of a 256-byte SPI injection buffer).
- **Input vs output is behavioral, not metadata.** Input ports carry commanded
  values (table → cache → C read); output ports carry firmware-produced values
  (C write → table). A signal has no direction metadata — the same port may do
  both; the table is the rendezvous.
- **When registrations become visible:** they buffer in the backend and become
  table entries when the member applies them — at `set_enabled(true)` (i.e.
  `Engine::add_member`) and at the start of each firmware tick. The typical flow
  (`start` → `add_member`) makes init-time ports visible before the first step,
  so routes into them validate and propagate immediately.
- **First user:** the sim `HW_ADC` registers one input port per enabled regular
  input (local name = the channel config's `inputNameStr`, unit `V`). A driven
  port commands that input's pin voltage; an undriven one keeps the synthetic
  ramp.
- **Output ports** publish driver-produced state out of the firmware. The sim
  `HW_TIM` is the first consumer: it registers one duty + one enable port per PWM
  channel plus a per-peripheral master output enable
  (`PWM_{U,V,W}_{duty,enabled}`, `TIM1_MOE`), publishing the commanded bridge
  state (normalized duty ∈ [0,1], 0/1 flags) event-driven from its setters — the
  D6 route source a motor model consumes.

### Comms entries (the framework is the wire)

Because the firmware runs in virtual space, the framework *is* the transport
for every serial bus. The sim-HW bus driver (e.g. sim `HW_USB`, sim `HW_SPI`)
calls a **C→Rust upcall** with each payload; the framework (1) records it in the
comms entry's history and (2) routes it to the destination (a peer model now, an
external transport — the real desktop app, D5 — later).

- **Request/response buses use DuplexTransfer** (landed for `spi`) — a **generic,
  engine-scoped primitive**. An engine-owned `DuplexRouter` couples any initiating
  [member](architecture.md#member-model) to a linked
  [`DuplexPeer`](architecture.md#member-model): the initiator runs a synchronous
  exchange (tx in, the peer's rx back within the same call), and the engine
  force-records it as `:tx`/`:rx` event entries after all members advance. A model
  initiates through `MemberCtx::duplex_transfer`; a **firmware** member is just one
  initiator among many — its C SPI upcall forwards into the **same** router. Endpoints
  are `spi:<owning-member>:<local>`; linking one nobody has declared yet is legal
  (a pending link that resolves when the endpoint is declared; a still-dangling link
  warns once). **Limitation:** the primitive is synchronous, so it needs a Rust-side
  responder (a peer/model). Two firmware instances on one bus (the multi-device stretch)
  cannot answer each other synchronously — firmware↔firmware duplex rides the **D8
  delayed-response** extension (`backlog.md`). Streaming buses (`usb_cdc` telemetry
  capture) still want the plain upcall→queue.
- **Logical payloads, not raw bytes.** We own the sim-HW driver code, so a comms
  entry holds the *logical contents* of the packet (a `Value::Record`/`Bytes`
  shaped by the transport), not a bitstream — far nicer to assert on and route.
- **Timing.** A synchronous DuplexTransfer delivers rx within the same tick. A
  non-blocking (DMA/IT) transaction's completion instead schedules a **one-shot
  interrupt** (D8) quantized to the base `dt`; rx is delivered then. Streaming
  capture + D8 timing remain future.
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
  mirrors** on their own clock: a `FirmwareMember` **flushes** the *fresh* (route-/
  test-written) `cvar` entries in its namespace into firmware memory and
  **sweeps** its whole leaf list back out around its firmware tick; a model reads
  a routed `vsig` input straight from the entry. The route is indifferent to which —
  no bespoke firmware code moves data. A route driving a `cvar` marks it dirty, so
  the consuming firmware member flushes it the same tick.
- **A destination is any registered signal of any `sig_type`.** A `cvar`
  destination (model output → a firmware sensor input, flushed by the consuming
  firmware member) and a `vsig` destination (a model input, read by the consuming
  model) work through the identical path — a `record` into the entry — with no
  per-`sig_type` restriction and no new seam. The table is a flat, member-agnostic
  registry.
- **Fault injection = suspend the route, then write the destination.** A live
  route re-drives its destination via `record` every tick, so a direct write would
  be clobbered. `suspend` the route and the destination stops being recorded; a
  `record` straight into that entry then persists (one-shot, last-writer-wins),
  and `resume` hands the destination back to the route. Composes with routing at
  **zero extra mechanism** — no pin, no override, just the ordinary write.
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

## 3. Tick evaluation order (SETTLED — per-route latency)

**The design: option B with annotations.** Discrete sim serializes concurrent
physics, so every feedback cycle needs *exactly one* tick of separation
somewhere. Rather than bake a uniform one-hop-per-tick delay into *all* routes,
the cut is made **explicit and physical per route**, and **forward dataflow gets
zero added latency**.

### Per-route latency

Every route carries a **`latency`** in engine ticks: **`0`** (default) or **`1`**.
(The field is a `u32` so higher values are representable later; anything `> 1` is
rejected today with a clear "not yet supported" error.)

- **Zero-latency (forward dataflow).** The destination receives the source's value
  **as produced this same tick**. A chain `a→b→c` of zero-latency routes resolves
  *fully* in one tick.
- **Latency-1 (the ZOH cut).** The destination receives the source's value **as of
  the end of the previous tick** — modelling the real zero-order-hold
  sample/actuation boundary of a sensor/actuator. This is the *one* explicit
  tick of separation a feedback loop needs.

The API keeps the common case terse: `add(src, dst)` is latency-0; a delayed edge
is `add_with_latency(src, dst, 1)` (voyant) / `Engine::add_delayed_route(src, dst)`
(the engine sugar).

### Engine step

```
per tick:
  1. now += grid_us; set_time(now)
  2. if wiring dirty: validate (below); cache the verdict + zero-latency topo order
  3. propagate DELAYED routes once — from a snapshot taken BEFORE any member
     advances (each delayed dst gets its source's end-of-previous-tick value)
  4. for each ENABLED member, in registration order:
       a. evaluate the enabled ZERO-latency routes in topological order with FRESH
          reads (a→b→c resolves fully, reading values produced earlier this tick)
       b. if the member's Cadence says it is due: member.advance(dt)
                               (a firmware member: flush fresh cvars →
                                advance_tick → sweep the whole cvar mirror out; a
                                model: read inputs, step, push outputs; OnDemand
                                members skip a+b — the bus drives them)
  5. record signals; asserts/injection; pace (realtime: sleep · fast: now)
```

Why this is correct:

- **Mutual exclusion holds.** Propagation is a pure State Table operation — it
  records between *entries* and never touches firmware memory. The firmware member
  syncs memory (flush the fresh `cvar` entries in, sweep the whole mirror back out)
  inside its own advance while the firmware is quiescent
  ([`ffi-boundary.md`](ffi-boundary.md) §5), so reading firmware-*output* statics
  and writing firmware-*input* statics is race-free.
- **Zero forward latency.** Re-evaluating the full zero-latency DAG (in topo order)
  before *each* member is semantically identical to per-member incoming-route
  resolution — routes are pure copies and `record` dedups unchanged values — but far
  simpler. So a member always sees the fully-resolved forward dataflow at its turn,
  with no artificial per-hop delay. (The `M×R` re-evaluation cost is a flagged perf
  seam — fine at current scale; [`performance.md`](performance.md) owns later
  optimization.)
- **The delayed edge is the physical cut.** A latency-1 route is where a feedback
  loop's one unavoidable tick of separation lives — a faithful ZOH sample. At the
  base `dt` (finest ISR rate, D6) this is sub-sampling-period and physically
  correct; zero-delay algebraic loops are ill-posed in discrete time anyway.

### Member registration order is a design surface

Because forward flow is resolved along **member registration order**, that order is
a deliberate design surface: **order members along the signal flow.** The validator
(below) tells you when you get it wrong — a route that reads a value a member has
not produced yet is a backward edge and must either be declared delayed or the
members reordered.

### Step-time validation (dirty-flag cached)

Wiring mutations (route add/remove/suspend/resume, member add, member
enable/disable) set a **dirty flag**; `step()` revalidates only when dirty, else
reuses the cached verdict + topo order. Rewire-at-any-time stays legal — a
validation failure surfaces at the **next** `step`, loudly, as an error naming the
offending route. Checks:

- **Zero-latency graph acyclic** (enabled zero-latency routes only). A cycle is an
  algebraic loop → error; declare one edge delayed to break it.
- **Forward flow.** Each signal gets an *availability index*: `avail(s) =
  ownerIndex(s)` if `s`'s `<source>` names a registered member, else `-1`
  (driver-owned — set between steps, available before all members); then propagate
  through the zero-latency DAG in topo order: `avail(s) = max(own,
  max(avail(src)))` over enabled zero-latency routes into `s`. For every enabled
  zero-latency route whose `dst` is owned by a member, require `avail(src) <
  ownerIndex(dst)` **strictly** (a same-member zero-latency loop is a silent delay
  and must be declared delayed). A violation errors: *"route X→Y needs latency
  (feedback/backward edge) or reorder members"*.
- **Single driver.** Two *enabled* routes with the same `dst` is an error
  (suspended routes exempt — fault-injection swaps stay legal).

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
- ~~**Model-update ordering** vs per-hop latency.~~ **Resolved (§3):** per-route
  latency (0 forward / 1 delayed ZOH cut); forward flow resolves fully each tick in
  topological order along member registration order, validated at step time.
- ~~**Auto-registration scope:** register *all* DWARF statics or filter to a
  project allowlist?~~ **Resolved:** the firmware member mirrors the **whole
  traceable namespace** by default (every scalar/enum leaf under every static),
  minus a built-in exclusion policy (array-size threshold drops stacks/heap/large
  buffers; pointers/functions/multi-dim skipped). Per-member
  `skip_cvar_registration_by_prefix(prefix)` / `register_cvar_in_state_table(path)`
  tune it; a general symbol/pattern trace filter is a later
  cosmetic addition ([`signal-trace.md`](signal-trace.md) §9).
- **Entry keying / path syntax** for nested members and array elements.
- ~~**Model registration API** (Rust): how models declare their entries.~~
  **Resolved:** open registration — a member registers its entries directly on the
  table at runtime (`StateTable::register`, idempotent), inside
  `Member::set_enabled` / `advance`. No `signals()` declaration.
