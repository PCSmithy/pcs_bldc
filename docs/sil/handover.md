# SIL bring-up — handover

Last updated: 2026-07-09. Orientation for picking up the SIL (software-in-the-
loop) effort in a fresh session. Read this first, then `README.md` +
`roadmap.md` in this folder.

## TL;DR

We're building **`voyant`** — a generic, firmware-agnostic framework that runs
the pcs_bldc firmware (cross-compiled to a host shared library) in a
deterministic virtual world, with total white-box access to its state.
**Phase 1 (firmware runs on the SIM target) and the Phase-2 core (Rust drives +
introspects the firmware) are done and working.** The design is fully decided
and captured (decisions D1–D12). What's left is the *engine* on top of the State
Table: models, the Route Table, the sim clock/step loop, comms, run modes, and
Python bindings.

All work is on branch **`sil`** (worktree `C:/code/pcs_bldc-sil`), pushed to
`origin/sil`. The branch was **rebased onto current `main`** (`c9b4238`, the
complete firmware foundation) on 2026-07-04, and the **firmware/SIL build was
unified**: the SIM target now runs the SAME four FreeRTOS tasks and real
io/dev/app modules as embedded (see "What's done").

## Build & run

```bash
tools/run_sil.sh            # RELEASE (default): -O3 -flto -g firmware DLL + --release voyant, run the sanity suite
tools/run_sil.sh --debug    # DEBUG: -O0 -g DLL + debug voyant (dev/introspection; slower, source-faithful)
tools/run_sil.sh --clean    # wipe the native build dir first
```

**Default is optimized on both sides** — the suite's performance numbers are only
meaningful optimized. Release and debug use separate build dirs
(`build/native-fw-release` vs `build/native-fw`) and cargo target dirs, so they
coexist. The dev/test flow (`tools/build_native.sh`, Unity) stays `-O0 -g`; the
release DLL rides a `PCS_OPT_LEVEL` cache knob (`-O3`) plus a `PCS_LTO` switch
(`-flto -ffat-lto-objects` compile + `-flto` link) in `native.cmake`, driven by
`build_native.sh --opt -O3 --lto` (mirrors the ARM toolchain's opt knob). The
suite prints a phase-isolated **performance report** at the end (firmware tick /
full step / engine floor / derived); see the dated baseline in `performance.md`
§11 and the -O3+LTO measurement in §14.

The suite loads the firmware DLL, drives it over the control ABI, and runs
named PASS/FAIL checks (exit nonzero on any failure): boot; all four real
FreeRTOS tasks advancing; State Table historian/enum/ZOH; and an end-to-end
path — inject an AS5048 SPI frame via DWARF write, assert the exact angle
comes out in the telemetry text captured by the sim USB driver. All
injection/inspection is white-box DWARF access (never the deprecated `_sim_*`
C APIs — see `backlog.md`).

Rust unit tests: `cd sw/sil && cargo test -p voyant` (59 tests).

**Rust toolchain gotcha:** it's `stable-x86_64-pc-windows-gnu` (matches MinGW),
installed at `~/.cargo/bin`. The Bash tool's shell does NOT source `~/.bashrc`,
so prefix cargo commands with `export PATH="$HOME/.cargo/bin:$PATH"`. (The
user's own terminals get it via `~/.bash_profile`.) See
`memory/reference_rust_toolchain.md`.

## Code map

```
sw/sil/
  voyant/                 THE FRAMEWORK (generic, no pcs_bldc code)
    src/signal.rs           SignalId (sig_type:source:name[:modifier]) + Value
    src/state_table.rs      StateTable: registry + per-signal change-log history
                            + current cache + overrides + per-signal epsilon
                            + retention. Pure data, no FFI. (10 unit tests)
    src/backend.rs          Firmware (public handle): control ABI + cvar
                            sample-resolver (start/shutdown/advance_tick +
                            read_cvar/write_cvar; the only unsafe/DWARF part).
                            An internal pub(crate) Backend trait is the
                            execution/test-double seam FirmwareMember drives.
                            Auto-derives the ASLR anchor from export∩DWARF (no
                            hardcoded symbol). Also FirmwareMember: a firmware
                            instance wrapped as a Member — AUTO-mirrors the whole
                            traceable cvar namespace (enumerated from DWARF at
                            enable, array-size exclusion policy + exclude/include),
                            flushing fresh/pinned cvars into fw memory + sweeping the
                            whole mirror back out around advance_tick; the ONLY thing
                            that touches fw memory (routes never do). dwarf.rs gained
                            leaf enumeration; state_table.rs a dirty set +
                            record_mirror/take_dirty/pinned.
    src/member.rs           Member trait (the one seam the engine drives everything
                            through: name/advance(dt,st)/set_enabled) + vsig_id +
                            RampModel reference model member. Members register their
                            own signals on the table (any time, any sig_type) and
                            push records each advance.
    src/route.rs            RouteTable: source->destination transport, one
                            snapshot-then-write pass/tick, add/remove/suspend/resume.
                            propagate is TABLE-ONLY (no backend): records src entry
                            -> dst entry; dst = any registered signal; both endpoints
                            checked at propagate; override pins a dest against a route.
    src/log.rs              Unified log: LogLevel/LogEntry + drop-oldest LogRing the
                            StateTable stamps with sim time (st.log/take_logs).
    src/dwarf.rs            DwarfMap: resolve var.member/arr[i] paths -> Leaf
                            (Scalar | Enum), incl. enum value->name
  pcs_bldc_sil/           THE INSTANTIATION (board-specific driver/demo)
    src/main.rs             loads the DLL, builds a StateTable, runs the demo
  spike/d1-tick/          standalone D1 spike: cooperative fiber FreeRTOS port
                            determinism + throughput test (throwaway scaffolding)

sw/lib/c/FreeRTOS/portable/Native-Fiber/   the native cooperative fiber port
sw/lib/c/shared/hw/sim/ports/              SIL_ports: the null-safe C helper sim
                                            drivers use to register/read/write ports
sw/fw/src/sil_fw.h                          the control ABI (setHooks/start/advance/shutdown)
sw/fw/src/main.c                            SIM path: fiber-port bring-up + ABI impl
sw/fw/src/hw/sim/FreeRTOSConfig.h           native FreeRTOS config
tools/run_sil.sh                            one-command build + run

docs/sil/*.md            the design (see "Design docs" below)
```

## What's done

- **Design (D1–D12): all decided + captured** in `docs/sil/`. Key choices:
  single-threaded cooperative **fiber** execution (determinism + perf); native
  firmware as a **shared library** driven in-process; DWARF white-box; **State
  Table + Route Table** with the State Table *being* the historian; comms
  captured as state via sim-HW upcalls; generic **`voyant`** framework +
  `pcs_bldc_sil` instantiation.
- **Phase 1 — firmware on the SIM target (done):** FreeRTOS runs on native via
  the hand-written cooperative fiber port; the firmware builds as
  `libpcs_bldc_fw.dll` exporting the `sil_fw_*` control ABI; both native and
  ARM targets build green. (D1 spike proved determinism + ~5000× realtime.)
- **Phase 2 core — Rust drives + introspects (done):**
  - `voyant::Firmware` loads the DLL, calls the control ABI, and reads/writes
    **any** firmware `static` by DWARF path — scalars, struct members, **array
    elements**, and **enums by symbolic name**.
  - **State Table** implemented: `SignalId`, logical `Value`, per-signal
    change-logged history (dedup + per-signal epsilon, default 1e-3), current
    cache (O(1)), `value_at` ZOH (O(log n)), injection **overrides**, time-based
    retention (`None` = unbounded for fast mode).
- **Rebased onto current `main` + firmware/SIL build unified (2026-07-04):**
  the SIM target runs the SAME four FreeRTOS tasks (`task_1ms`, `task_10ms`,
  `task_usb`, `telemetryTask`) and the same io/dev/app init as embedded, via
  shared `prvHwInit`/`prvAppInit`/`prvCreateTasks` in `main.c`. Target gating
  survives only at the hw-layer seam plus `HAL_Init`/timebase/`__io_putchar`
  and the per-target entry points. All io/dev/app modules link on both targets
  against real sim hw drivers; sim `HW_USB_run()` yields via `vTaskDelay(1)`
  (interim until D8). Per-task heartbeat counters + the sanity suite (above)
  prove the real code runs natively. Embedded ELF impact: +~80 B flash,
  +16 B bss.

- **Member-model refactor (chunk A, 2026-07-05):** the `Model` trait folded into a
  single **`Member`** seam (`name`/`advance(dt,st)`/`set_enabled(on,st)`) that the
  engine drives everything through; it now holds a `Vec<Box<dyn Member>>` and
  advances in registration order. `RampModel` and the firmware (via the new
  **`FirmwareMember`**) are both members; members register their own signals on the
  table and push records each advance (no `signals()`/`read()`). `StateTable::register`
  is now idempotent (identical re-registration is a no-op; conflicting unit errors).
  The ASLR anchor is **auto-derived** (export∩DWARF) — no board symbol in voyant.
  Deleted: `Model`/`ModelSignal`/`register_model`/`record_model`, `Firmware::read_u32`,
  the engine's pull-based vsig cache + `Engine::sample_cvar`.

- **Table-mediated routing + log system (chunk A follow-up, 2026-07-05):** routes
  are now a **pure State Table operation** — `RouteTable::propagate(&mut StateTable)`
  takes **no `Backend`**; it records source entries into destination entries and
  nothing else. A destination is **any registered signal of any `sig_type`** (the
  `cvar`-only restriction and `RouteError::UnsupportedDest` are gone), so `vsig`
  destinations (model inputs) work with no new seam, and `set_override` on a
  destination pins it against its route (free fault-injection compose). Added
  `RouteTable::remove`; both endpoints are existence-checked at propagate
  (symmetric). The firmware member gained **`drive_cvar`** — the mirror of
  `sample_cvar`: per firmware tick it flushes driven cvars (table -> fw memory) ->
  `advance_tick` -> samples sampled cvars (fw memory -> table). The **`Engine`
  dropped its `&dyn Backend`** entirely (`Engine::new(tick_period_us)` /
  `with_state(tick_period_us, st)`) — it touches only members/routes/table; each
  firmware member drives its own backend, so multi-firmware is just multiple
  `FirmwareMember`s. Added the **unified log system** (`log.rs`: `LogLevel`/`LogEntry`
  + drop-oldest `LogRing`; the `StateTable` owns the sink and stamps sim time via
  `st.log`/`take_logs`); the swallowed-`record`-error sites now log a `Warning`.
  Sanity-suite **check 6 moved onto the engine** and now exercises the real
  production path (model vsig -> route table->table -> FirmwareMember flush -> fw
  memory), asserting against the SPI sim's `injectedRx[0]` — a firmware input the
  firmware *reads* but never *writes*, so a flushed value survives `advance_tick`.
  voyant unit tests 34 -> 43; sanity suite all PASS.

- **Route latency + step-time validation (settled "B with annotations", 2026-07-05):**
  routes gained a **per-route `latency`** (0 = same-tick forward dataflow, 1 = the
  delayed ZOH sample/actuation cut; `u32`, `>1` rejected). `RouteTable::propagate`
  split into **`propagate_delayed`** (snapshot-then-write, once at tick start, from
  end-of-previous-tick values) and **`propagate_zero_latency`** (fresh reads in
  cached topological order, re-run before each member so a chain `a→b→c` resolves the
  SAME tick — the old one-hop-per-tick defect is gone). New **`RouteTable::validate`**
  (given member names in registration order) enforces: single-driver (enabled routes;
  suspended exempt), zero-latency-graph acyclic, and forward-flow (availability-index
  along member order) — errors `MultiDriver`/`Cycle`/`BackwardRoute`/`UnsupportedLatency`.
  The **`Engine` caches the verdict + topo order behind a dirty flag** set by every
  wiring mutation; a bad verdict is raised at the next `step` and re-raised until
  fixed (rewire-at-runtime stays legal). API sugar: `Engine::add_delayed_route`.
  **Member registration order is now an explicit design surface.** Sanity-suite
  **check 7** added: a genuine two-member feedback loop (model `out` → firmware
  `injectedRx[0]` zero-latency; firmware `task1msRuns` → model `in` delayed) — the
  validator rejects the loop until the backward edge is declared delayed, then the
  loop steps to an exact predicted sequence. voyant unit tests 43 -> 53; sanity suite
  all PASS.

- **Port registration seam (chunk B, 2026-07-05):** firmware members now expose
  **ports** — signals their sim HW drivers register with voyant at runtime, in
  **native format** (volts stay volts; the driver owns conversion to its C
  representation). The control ABI gained **`sil_fw_setHooks`** (installed by
  `Firmware::load`, before `start`): a vtable of `registerSignal` /
  `readSignal` / `writeSignal` trampolines targeting a RefCell'd port state
  inside the `Firmware`. The C side wraps it in the **null-safe `SIL_ports`
  helper** (`sw/lib/c/shared/hw/sim/ports/`, target `hw_SIL_ports`; no hooks →
  register invalid / read false / write no-op, so standalone + Unity runs are
  untouched). Port I/O is **cache-mediated like the cvar mirror lists** — per
  firmware tick the `FirmwareMember` runs three fixed phases over its signal
  bindings (ports, driven/sampled cvars): **in-sync** (apply pending
  registrations, id = `{sig_type}:{member}:{local}` — C never knows its instance
  name; fill every port's input cache from the table, never-driven → C read
  false → driver fallback; flush driven cvars in) → **`advance_tick`** →
  **out-sync** (drain the port-write buffer into the table; sample sampled
  cvars out). Registrations become visible at
  `set_enabled(true)` (= `add_member`) or the next firmware tick. First user:
  **sim `HW_ADC`** registers one input port per enabled regular input
  (`inputNameStr`, unit V) and converts a driven port's volts → counts via its
  own numBits/vref; undriven inputs keep the synthetic ramp. Sanity-suite
  **check 8**: model volts → route → `vsig:pcs_bldc:ADC1_IN6` → exact
  quantized counts by DWARF, with a neighboring input still ramping. ARM build
  byte-identical (SIM-only C). voyant unit tests 53 -> 58; sanity suite all
  PASS. The `_sim_*` removal (backlog) now has its input-injection replacement.

- **Route-validation fix + Backend demotion (2026-07-05):** fixed a forward-flow
  validation bug — availability now folds through the zero-latency DAG in
  *topological* route order (was insertion order, which under-propagated through
  chains of ≥2 unowned intermediates and let a transitively-backward route pass
  validation). Demoted the `Backend` trait and `PortDef` to `pub(crate)`:
  **`Member` is THE public seam**, `FirmwareMember` wraps the concrete public
  `Firmware` handle, and `Backend` is internal execution/test-double plumbing
  (lifecycle `start`/`shutdown` are now inherent `Firmware` methods, off the
  trait). voyant unit tests 58 -> 59; sanity suite all PASS.

- **Whole-namespace cvar mirror (2026-07-07):** collapsed the explicit
  `drive_cvar`/`sample_cvar` declarations (both **deleted**) into **automatic
  whole-namespace cvar mirroring** — the documented D12 end-state. At enable a
  `FirmwareMember` enumerates every traceable leaf from DWARF (`dwarf.rs`
  `enumerate_leaves`: recurse structs, expand arrays, **default array-size
  threshold 32** to drop stacks/heap/512-byte buffers, multi-dim/pointer skip,
  depth/leaf cap) and registers `cvar:<member>:<leaf>` for each, caching a resolved
  address/type handle per leaf. Out-sync **sweeps** them all memory→table
  (`record_mirror`) each tick; in-sync **flush is sparse** — a State Table **dirty
  set** (`record`/`force_record`/pin mark dirty, `record_mirror` does not) drained
  per-source (`take_dirty`) ∪ pinned ids, filtered to `cvar`. A pinned cvar
  re-asserts every tick (fault-injection drive); a mirror record on a pinned entry
  is ignored (never un-pins). `exclude(prefix)`/`include(path)` tune the policy
  (the suite `include`s the one 256-byte-buffer SPI MISO byte it drives). Sweep
  cost on the pcs_bldc DLL: **~430 leaves/tick** (Lever-4 dirty-page-scan
  workload). voyant unit tests 59 -> 70; sanity suite 10 checks all PASS (added a
  mirror-accuracy check on `HW_ADC_data.tickCounter` — no declaration).

- **Sweep perf — Tier 1 + Tier 2 (2026-07-09):** the whole-namespace mirror sweep
  went from a naive per-leaf scan to a **shadow-snapshot `memcmp` sweep** (Tier 1:
  resolved leaves grouped into address ranges → 64 B chunks with per-range shadow
  buffers; per tick each range is `memcmp`d against live memory and only changed
  chunks re-decode their leaves — O(changed bytes)) over a **dense-index State
  Table fast lane** (Tier 2: hot per-signal storage is index-keyed `Vec`/`usize`
  sets; sweep/route/port hot paths resolve their index once and use
  `record_mirror_at`/`record_at`/`current_value_at`, so no hot path hashes a
  `SignalId` — public string API unchanged). **Full engine step 54 → ~9.3 µs
  (18× → ~107× realtime)**, meeting the owner target (≤10 µs). voyant unit tests
  70 → 74 (4 shadow-sweep tests); sanity suite (release + debug) all PASS, behavior
  identical. See `performance.md` §5 (levers marked implemented) + §12 (after
  table).

- **Columnar historian — D12 storage end-state (2026-07-09):** the per-signal
  change-log moved from `VecDeque<(u64, Value)>` to **per-signal typed columns**
  (`times` + a native scalar deque, kind fixed at first record) with a **boxed
  `(u64, Value)` column** for `Enum`/`Bytes` signals (strict per structured
  variant). A signal has exactly one `Value` type for its lifetime, so a later
  variant mismatch is a **bug**: it is rejected with `TableError::TypeMismatch`
  (no migration) — a mis-typed route fails `step()` via `RouteError::Table`, a
  mirror sweep logs a Warning and continues. The sweep's changed scalar leaves
  record through **typed fast lanes**
  (`record_mirror_<t>_at` + a native `read_cvar_scalar` decode) that compare the
  column tail natively — no `Value` on the hot path. Per-sample footprint dropped
  ~2.5–3.3× (f64 40→16 B, u32 40→12 B); full step ~8.9 µs (~112×), sweep+flush
  ~3.1→~2.8 µs. **One public ripple (owner-accepted):** `current_value`/`value_at`
  return `Option<Value>` **by value**, `changes` returns `Option<Vec<(u64, Value)>>`
  (materializing). voyant unit tests 76 → 83 (7 columnar tests). See
  `signal-trace.md` §1 + `performance.md` §6/§13.

## What's next (prioritized)

> **Current sprint (2026-07-12): full-loop motor commutation** — see
> `roadmap.md` § "Current sprint" for the staged plan (string-keyed table
> write API → SPI comms seam → encoder model → PWM ports → motor/inverter →
> harness → closed-loop scenario). D8 is deferred to the following
> (interrupt-driven-control) sprint; `usb_cdc`/`teleplot` telemetry capture
> is filed near the top of `backlog.md`.

1. ~~**`Model` trait + `vsig` backing**~~ — **DONE (2026-07-04).** `voyant::model`
   adds the minimal `Model` trait (`name`/`signals`/`advance(dt_us)`/`read`), the
   `ModelSignal` descriptor, and `register_model`/`record_model` glue that samples
   a model into the State Table exactly like cvars (StateTable stays pure data).
   `RampModel` is the reference impl; the sanity suite check 5 demonstrates
   register → advance-with-time → historian record. Plant models come later
   (Phase 3, instantiation-side).
2. ~~**Route Table**~~ — **DONE (2026-07-04).** `voyant::route` adds `RouteTable`
   (`add`/`suspend`/`resume`/`propagate`): a flat list of `source → destination`
   routes propagated in one **snapshot-then-write** pass per tick (snapshot all
   enabled sources from the State Table's current cache, then write all dests), so
   a chain `x→y→z` advances one hop per tick. Sources are any State Table entry
   (`vsig`/`cvar`); destinations are `cvar`s driven via `Backend::write_cvar` (the
   DWARF path is the id's `name` segment — no separate mapping). Per-route
   `suspend`/`resume` gates driving for fault injection (pairs with the table's
   `override`). A `vsig` destination (model input) needs a `Model::write` seam and
   is rejected at `add` for now. 8 unit tests (add/propagate/suspend/resume/
   snapshot-consistency + a RampModel→cvar route); sanity-suite check 6 routes a
   model's `vsig` into a firmware `cvar` and proves suspend/resume gating.
3. ~~**Sim clock / step loop**~~ — **DONE (2026-07-04).** `voyant::engine` adds
   `Engine`: it owns the State Table / Route Table / models, borrows a `Backend`,
   and `step()`s the canonical order per tick — advance sim time (`now +=
   tick_period`, monotonic/wall-clock-free) → advance models in registration order
   + record their `vsig`s → propagate routes → `advance_tick` → sample registered
   firmware `cvar`s into the historian. Sampled-cvar registry via `sample_cvar`;
   models via `add_model`; routes via `add_route`/`suspend_route`/`resume_route`.
   Perf seams baked in: each model's vsig ids + the sampled-cvar list are resolved
   once, so the hot loop never calls `Model::signals()` (no per-tick alloc); the
   remaining per-tick allocs (route `pending`, enum/bytes `Value`) sit behind seams
   owned elsewhere and don't touch the engine API. 7 unit tests (step ordering via
   a call-order mock backend, time advance, multi-model determinism, sampled-cvar
   + vsig recording, empty step, unregistered-source error). The **sanity suite now
   drives through the engine** for checks 2/3/4/5 (tasks, historian/ZOH,
   end-to-end, model vsig); checks 1/6/7 stay below it (backend lifecycle; and 6
   reads the route-written cvar *between* propagate and advance_tick, a
   finer-than-step granularity).
4. **Formalize the trait seams** — `Backend` and `Model` **DONE (2026-07-04):**
   `voyant::Backend` (lifecycle `start`/`advance_tick`/`shutdown` + `cvar`
   read/write) is extracted, with `Firmware` as its first impl; `Model` above.
   `Transport` / `Scenario` remain (later chunks).
5. **D8 interrupt controller** — the C→Rust upcall registration + port dispatch
   shim; needed before the fast control ISR / comms fire.
6. **Comms** — the `Transport` trait + sim-HW upcall capture (logical payloads),
   routed to peer models (external transport / desktop app later, D5).
7. **Run modes** (fast/realtime pacing) + **Python bindings** (pytest, D3) +
   **dashboard** (D4). **Perf seams** from `performance.md` (zero-alloc hot loop,
   gated discrete work, dirty-page historian scan) — bake in as the loop grows.

## Open threads / pending decisions

- **Trait seams — `Backend` + `Model` done; `Transport` + `Scenario` remain.**
  `voyant::Backend` (with `Firmware` as first impl) and `voyant::Model` (with
  `RampModel` reference impl) are formalized (2026-07-04). The generic-framework
  vision (`architecture.md` §7) still wants `Transport` (comms) and `Scenario`
  (wiring), which land with their chunks (#6 / run-config).
- **Coercion follow-ups (low priority, accepted):** `i64` firmware fields narrow
  to `Value::I32` (rare; user OK with it); unknown enumerators read as `<n>`.
- **Deferred cleanups live in `backlog.md`** — notably: remove the
  `HW_<Module>_sim.h` inject/inspect layer (redundant once SIL white-box
  injection is feature-complete) and rework the unit tests that lean on it.

## Design docs (source of truth)

In `docs/sil/`: `README.md` (index) · `architecture.md` (the picture + decisions
table) · `roadmap.md` (phase-by-phase status) · `state-route-tables.md`
(State/Route Table + naming + comms) · `signal-trace.md` (D12 historian) ·
`freertos-tick.md` (D1 fiber port) · `ffi-boundary.md` (D2) ·
`sim-interrupts.md` (D8) · `performance.md` (perf levers) · `inverter-timestep.md`
(D6) · `time-virtualization.md` (D9) · `determinism.md` (D7).
