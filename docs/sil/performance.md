# Performance architecture

Cross-cutting design doc. The sim must run **many× faster than realtime on a
normal desktop** — no supercomputer. This captures the cost model, the levers,
and the seams we bake in from the start so the target stays reachable.

---

## 1. Cost model

Base `dt` is fine (a few µs), so 1 sim-second = ~10⁵–10⁶ ticks. To run **N×**
realtime, each tick must cost `dt/N` (e.g. `dt=2µs`, 50× → **40 ns/tick**). The
whole game: **make the common tick ≈ one ODE step, and make ticks rarer.**
Per-tick cost centers:

1. continuous-model ODE integration — *every* tick
2. framework↔firmware boundary crossing — *every advance*
3. firmware execution + task context switches — *firmware-active ticks*
4. historian change-detection — *trace-everything*
5. route propagation

## 2. Lever 1 — single-threaded cooperative fibers (the foundation)

**One OS thread runs everything.** The framework is the "main" context; each
FreeRTOS task is a **fiber** (its own stack); the scheduler picks the next fiber;
`portYIELD` = a fiber swap (~30–100 ns). "Advance the firmware" swaps into the
firmware; quiescence (the idle fiber) swaps back. **No OS threads, no
cross-thread signaling.**

This replaces an earlier two-thread + OS-thread-host-port design, which was a
performance trap: OS-thread context switches are ~1–5 µs and a two-thread
framework↔firmware handshake signals ~1 µs/tick — at 10⁵–10⁶ ticks/s that alone
can be **slower than realtime**. Fibers are ~50× cheaper *and* strictly more
deterministic. Foundational and hard to retrofit → **build it this way from the
start.** See [`freertos-tick.md`](freertos-tick.md) (D1).

## 3. Lever 2 — gate discrete work to when it's needed

Between control activations (e.g. 24 of every 25 ticks if control is 50 µs and
`dt`=2 µs) the firmware doesn't run and the PWM command is held, so the **only**
necessary work is integrating the continuous (dynamic) models with held inputs.
Everything else gates to firmware-active ticks:

- firmware execution, route propagation, historian scan → only when firmware ran
- **algebraic (stateless) models** (sensors, encoder, inverter) → evaluated
  **on-demand** when their output is read, not every tick
- **dynamic (stateful) models** (motor electrical + mechanical) → integrate every
  tick

Steady-state per-tick cost collapses to ≈ one ODE step. Free to design in now,
painful to bolt on later.

## 4. Lever 3 — fewer ticks via better integrators (couples D6)

Base `dt` is set by model *stability*. An **unconditionally-stable** integrator
(semi-implicit / exponential for the stiff electrical `di/dt`) makes `dt`
accuracy-limited, not stability-limited → potentially **~10× fewer ticks**. Also
avoid per-tick transcendentals — update the Park/Clarke `sin/cos` **incrementally**
(rotate by the angular delta) instead of calling `sin/cos` each tick. Tick count
is the master knob; the integrator is model-owned ([`inverter-timestep.md`](inverter-timestep.md))
so it can be upgraded without touching the engine.

## 5. Lever 4 — historian: gate + dirty-page tracking

Trace-everything ([`signal-trace.md`](signal-trace.md)) must not scan thousands
of statics every tick.

- **Gate on "firmware ran this tick"** (free): firmware statics only change when
  firmware executed, so skip the scan on idle ticks.
- **Dirty-page tracking:** mark the firmware's `.data`/`.bss` **read-only** before
  a burst; a fault handler (Windows **VEH** / macOS **mach exception**) marks
  written pages dirty and un-protects them; after the burst, scan only the traced
  variables on **dirty pages** (~100× less). `GetWriteWatch` does *not* apply
  (loader-mapped memory), so it's protect-and-fault.

Bake the **pluggable change-detector seam** now (naive-scan ↔ dirty-page); gate
first, add dirty-tracking when it profiles hot.

**The whole-namespace mirror sweep is exactly this workload.** The firmware
member now mirrors every traceable cvar leaf memory→table each tick (naive scan,
[`signal-trace.md`](signal-trace.md) §4) — this is what the dirty-page lever will
optimize. Two mitigations already hold: the sweep reads through **pre-resolved
address/type handles** (cached at enable — no per-tick DWARF re-resolution), and
the in-sync **flush is sparse** (only the command-dirtied cvars, via the
State Table dirty set, never the whole namespace). Measured on the pcs_bldc DLL:
**~430 cvar leaves** swept per tick (the built-in array-size exclusion drops the
task stacks / heap / 512-byte buffers that would otherwise dominate).
Phase-isolated measurement (§11) shows the **sweep dominates a full engine step**:
a bare firmware `advance_tick` is only ~4 µs, while the whole-namespace mirror +
flush + routes add ~45 µs on top — so **gated / dirty-page scanning is the
highest-value next lever**, exactly as this section predicts (an earlier note here
had it backwards, having measured the sweep folded into an *unoptimized*
`advance` and mistaken it for firmware cost).

**Implemented — Tier 1 + Tier 2 (2026-07-09).** The naive per-leaf scan is
replaced by two composed optimizations, taking the full step **49 → ~9 µs
(>100× realtime)**:

- **Tier 1 — shadow-snapshot `memcmp` sweep** (`backend.rs` / `FirmwareMember`).
  At enable, the resolved leaves are sorted by address and grouped into contiguous
  **ranges** (merged across gaps ≤64 B so a struct's traced fields share a range),
  each subdivided into 64 B **chunks** with a chunk→leaf map and a per-range
  **shadow buffer**. Per tick, each range is `memcmp`d against its shadow *directly
  against live memory* (no copy); only a **changed** range is pulled in and
  localized chunk-by-chunk, re-decoding just the leaves in changed chunks (a leaf
  straddling a chunk edge is listed under every chunk it overlaps). The shadow
  mirrors **memory** — updated even where the table dedups the
  record — and the first sweep after (re)enable is cold (full baseline). This is
  the gate this section called for: work is now **O(changed bytes)**, not
  O(leaves). (The address ranges derive strictly from resolved leaf `addr+size`,
  so every byte read lies within a real firmware static; see the `read_range`
  SAFETY note.)
- **Tier 2 — dense-index State Table fast lane** (`state_table.rs`). The
  `IndexSet<SignalId>` already yields a stable dense index per signal; the hot
  per-signal storage (`current`, `changes`, resolved `epsilon`, dirty/
  evicted membership) migrated to index-keyed `Vec`/`HashSet<usize>` storage, and
  the changed-leaf decode path, route endpoints, and port-cache fill resolve their
  index **once** (at registration / first propagation) and call the crate-internal
  `record_mirror_at` / `record_at` / `current_value_at` — so no hot path hashes a
  `SignalId` string. The public string-keyed API is unchanged (thin
  resolve-then-delegate wrappers).

Dirty-page tracking (the remaining §5 lever) is now **lower priority** — the
shadow `memcmp` already reads only the mirrored bytes and re-decodes only the
changed ones; dirty-page protect-and-fault would mainly cut the residual
whole-shadow `memcmp` traffic, a smaller win from here.

## 6. Lever 5 — zero-alloc hot loop

No `malloc` per tick. Preallocate everything; historian append = bump-pointer
into **chunked per-signal columnar buffers**; routes as flat `(src, dst, size)`
arrays iterated with `memcpy`. Steady-state allocation-free. Easy to design in,
miserable to retrofit.

**Columnar historian storage — implemented (2026-07-09).** The per-signal
change-log is now a **typed column** (`times: VecDeque<u64>` + a native value
deque, kind fixed at first record) with a **boxed `(u64, Value)` column** for
`Enum`/`Bytes` signals — the D12 storage end-state
([`signal-trace.md`](signal-trace.md) §1). Scalars store ~12–16 B/sample vs the old
`(u64, Value)` pair's ~40 B, and the sweep's changed scalar leaves record through
**typed fast lanes** (`record_mirror_<t>_at`, fed by a native `read_cvar_scalar`
decode) that compare against the column tail natively — **no `Value` construction
on the hot record path**. This holds sweep memory + speed roughly flat as Phase-3
multiplies signal counts and capture lengths (fast-mode retention is unbounded).

**Known seam — `M×R` zero-latency route re-evaluation.** The settled step
([`state-route-tables.md`](state-route-tables.md) §3) re-resolves the full
zero-latency route DAG (in cached topological order) before *each* of the `M`
members, so a member always sees fully-resolved forward dataflow. That is `M×R`
pure copies per tick where `R` is the zero-latency route count. It is correct and
simple, and fine at current scale; the optimization (resolve once per tick and let
each member read its already-final inputs, or per-member incoming-route slices) is
deferred here — the topo order is already cached across ticks (rebuilt only when the
wiring is dirty), so only the copies remain.

## 7. Lever 6 — parallelism (free aggregate throughput)

Fast mode scales across **processes** (pytest-xdist): aggregate = per-sim speed ×
cores. Single-threaded-fiber sims are ideal — one core each, zero contention.

## 8. Seams to bake in from the start

1. **Single-threaded fiber execution engine** (Lever 2 §2) — hard to retrofit.
2. **Continuous-vs-discrete split + gating** (Lever 3 §3) — hard to retrofit.
3. **Zero-alloc hot loop + columnar historian** (Lever §6) — hard to retrofit.
4. **Pluggable change-detector** naive-scan ↔ dirty-page (§5) — seam now, impl later.
5. **Model-owned integrator** (§4) so it can be upgraded.

## 9. Target

With levers 1–2 in place and 3–5 designed-for: **10–50× realtime on a laptop** is
realistic. Without lever 1: ~1× or worse.

## 10. Couplings

- **D1** — execution engine *is* the fiber model.
- **D2** — the boundary crossing is a fiber swap, single-thread.
- **D8** — ISR dispatch is cooperative, in the firmware context.
- **D9** — firmware bursts are instantaneous in sim time ⇒ no mid-task
  preemption needed (Lever 1 is *sufficient* and faithful in sim-time).
- **D6** — integrator choice sets the tick count.
- **D12** — historian cost (§5).

## 11. Measured baseline — Tier 0 (2026-07-09)

Phase-isolated, from the SIL suite's performance report (`pcs_bldc_sil`,
`report_performance`), averaged over 1000 ticks after warm-up on the dev laptop.
Two build configurations, **1 ms sim tick**; ×realtime = 1000 µs ÷ (µs/tick).
~430 cvar leaves mirrored per tick.

| phase                         | debug Rust + -O0 DLL | release Rust + -O2 DLL |
|-------------------------------|---------------------:|-----------------------:|
| firmware `advance_tick` alone |        5.9 µs (169×) |          4.1 µs (242×) |
| full engine `step` (measured) |        496 µs (2.0×) |            49 µs (20×) |
| empty engine `step` (floor)   |             0.06 µs  |               0.01 µs  |
| derived (full − firmware)     |              490 µs  |                 45 µs  |

**Findings.** (1) The old "~443 µs/advance" baseline was the *unoptimized* full
step; building the Rust framework `--release` and the firmware DLL at `-O2 -g`
takes the full step **496 → 49 µs (~10×)** and the suite to **~20× realtime** with
no hot-path work. (2) The firmware itself is cheap (~4 µs); the cost lives in
voyant's **whole-namespace mirror sweep** (the `derived` row), so Tier 1/2 should
target the historian scan (Lever 4 — gate on "firmware ran" + dirty-page tracking,
§5), not the firmware. (3) The engine's own per-step overhead is negligible
(floor ≈ 0). Reproduce with `tools/run_sil.sh` (release) and
`tools/run_sil.sh --debug`.

## 12. After Tier 1 + Tier 2 (2026-07-09)

Same suite / method, after the shadow-`memcmp` sweep (Tier 1) + dense-index State
Table (Tier 2). **1 ms sim tick**, ~429 cvar leaves, release Rust + -O2 DLL.

| phase                              | before (Tier 0) | after (Tier 1+2) |
|------------------------------------|----------------:|-----------------:|
| firmware `advance_tick` alone      |    4.1 µs (244×) |     ~4.2 µs (237×) |
| **full engine `step` (measured)**  |     **54 µs (18×)** | **~9.3 µs (107×)** |
| firmware-member step (sweep+flush) |               —  |     ~7.6 µs (131×) |
| derived (full − firmware)          |           50 µs  |          ~5.2 µs |
| ↳ shadow sweep + flush             |               —  |          ~3.3 µs |
| ↳ model + route + propagate        |               —  |          ~1.8 µs |

**Result: the owner target (full step ≤10 µs, >100× realtime) is met** — full
step ~9.0–9.4 µs run-to-run (~107× realtime), a **~6× speedup** over Tier 0's
54 µs. The remaining ~5.2 µs of voyant work splits ~3.3 µs shadow sweep+flush
(whole-shadow `memcmp` + decode/record of the ~30-60 actually-changing ADC/counter
leaves) and ~1.8 µs model+route+propagate (the `M×R` zero-latency re-eval + the
model's per-tick record). Both are now well within the ~6 µs/tick budget; the
report gained two breakdown rows (`firmware-member step` and the two `of which`
lines) to keep this attribution live and cheap.

## 13. After columnar historian (2026-07-09)

Same suite / method, after replacing the per-sample `VecDeque<(u64, Value)>`
change-log with **per-signal typed columns + a boxed fallback** (§6) and routing
the sweep's changed scalar leaves through **typed record fast lanes**. **1 ms sim
tick**, ~429 cvar leaves, release Rust + -O2 DLL.

| phase                              | before (Tier 1+2) | after (columnar) |
|------------------------------------|------------------:|-----------------:|
| firmware `advance_tick` alone      |          ~4.2 µs  |         ~4.1 µs  |
| **full engine `step` (measured)**  |     **~9.0 µs**   |    **~8.9 µs**   |
| firmware-member step (sweep+flush) |          ~7.3 µs  |         ~7.0 µs  |
| ↳ shadow sweep + flush             |          ~3.1 µs  |         ~2.8 µs  |

**Result.** The full step holds at ~8.9 µs (~112× realtime) — the target stays
met. The honest, attributable win is in the **sweep+flush** row (~3.1 → ~2.8 µs,
run-to-run): the changed scalar leaves now compare natively against the typed
column tail and store 12–16 B/sample instead of ~40 B, so the per-record work
shrank. The bigger point is **memory + scaling**: per-sample footprint dropped
~2.5× (f64: 40 → 16 B) to ~3.3× (u32: 40 → 12 B), which is what keeps unbounded
fast-mode capture flat as Phase 3 multiplies signals and capture lengths. The one
public ripple (owner-accepted): `current_value`/`value_at` return `Option<Value>`
by value and `changes` materializes `Vec<(u64, Value)>` — columnar storage cannot
hand out a `&Value`.

## 14. Release DLL to -O3 + LTO (2026-07-09)

The release SIL flavor (`tools/run_sil.sh` default) moved from **`-O2 -g`** to
**`-O3 -flto -g`** for the firmware DLL. `-flto -ffat-lto-objects` is added at
compile and `-flto` at link via a `PCS_LTO` switch in `native.cmake` (fat objects
keep concrete machine-code symbols so the SHARED library's `--whole-archive` /
`--export-all-symbols` and the DWARF-read statics all survive LTO). The `-O0 -g`
dev/debug flavor and the ARM build are untouched; no `-ffast-math` (FP semantics
unchanged). Same suite / method, release Rust, **1 ms sim tick, 429 cvar leaves**
(leaf count unchanged from §13 — LTO did not perturb the DWARF or elide a static).

| phase                              | before (-O2, §13) | after (-O3 -flto) |
|------------------------------------|------------------:|------------------:|
| firmware `advance_tick` alone      |          ~4.1 µs  |     ~2.9 µs (run 2.6–3.2) |
| **full engine `step` (measured)**  |       **~8.9 µs** |     **~7.6 µs (7.5–7.9)** |
| firmware-member step (sweep+flush) |          ~7.0 µs  |          ~6.1 µs  |

**Result.** The gain is firmware-side, as expected: `advance_tick` **~4.1 → ~2.9 µs
(~30%)**, which flows through to the full step **~8.9 → ~7.6 µs (~117× → ~132×
realtime)**. The voyant-side rows (sweep+flush, model+route) are unchanged work and
move only with run-to-run variance; the derived Rust cost is untouched by the DLL
flags. Debug flavor stays `-O0 -g` (430 leaves), all sanity checks PASS.

**LTO is Windows-only (2026-07-10).** *(the fine-grid re-baseline is §15)* The `-flto` half of this flavor applies on
**Windows (MinGW/GNU) only** — the `native.cmake` `PCS_LTO` flags are gated behind
`CMAKE_HOST_WIN32`. On Linux the GNU `-flto` ELF `.so` links but emits an **empty
DWARF map** (gimli reads 0 DIEs → the SIL reader finds no anchor), and macOS `gcc`
is Apple clang (no `-ffat-lto-objects`/plugin), so both keep `-O3` without LTO.
The Linux LTO+DWARF investigation is deferred (`backlog.md`); these perf numbers
are the Windows release flavor.

## 15. Opt-in fine grid + gated mirror (2026-08-15)

The engine grid became a **per-world choice** (`Sil::options().grid_us(50)`), with the
1 ms default untouched, and the whole-namespace mirror sweep moved onto a **sim-time
cadence** instead of running on every dispatching step. This is the §3 "gate discrete
work" / §5 "gate the historian scan" lever, finally exercised by a grid fine enough to
need it: at 50 µs (the 20 kHz PWM period) a control interrupt is due on *every* step,
so without a gate every step would pay the whole-namespace sweep.

**The gate.** `FirmwareMember` sweeps when `now - last_sweep >= sweep_period_us`
(default 1000 µs, `set_sweep_period_us`, world-level via `SilOptions::sweep_period_us`)
or when its shadow is cold. Everything else stays per-step / per-dispatch: route
propagation, port I/O, duplex, models, and the **in-sync cvar flush** — an inbound
value still reaches firmware memory before the firmware runs, so the gate never delays
the plant→firmware direction. Because the shadow mirrors *memory*, a withheld sweep
**delays** a record and never drops one: whatever differs from the shadow is picked up
whenever the sweep next runs. `Engine::mirror_now()` forces a sweep for an assert.

Two properties follow from measuring the cadence in sim time rather than in steps:
on any grid at least as coarse as the cadence, *every* dispatching step still sweeps —
so the whole default-grid suite behaves exactly as before — and the historian's cvar
latency bound is stated in sim time, independent of the grid a scenario chose.

**Coarse grid — re-baseline** (same suite / method as §14; 1 ms grid, 564 cvar leaves,
release Rust + `-O3 -flto` DLL; three back-to-back runs of each build on one laptop).

| phase                              |          before | after (gate) |
|------------------------------------|----------------:|-------------:|
| firmware `advance_time` + systick  | 0.90–0.93 µs    | 0.87–0.89 µs |
| **full engine step (measured)**    | **7.83–7.93 µs** | **7.68–7.84 µs** |
| firmware-member step (sweep+flush) | 6.35–6.59 µs    | 6.26–6.63 µs |

Unchanged within run-to-run noise, as the gate design requires: at a 1 ms grid the
1 ms cadence elapses between any two dispatching steps, so the sweep still runs on each.

**Fine grid — new baseline** (50 µs grid, same members and route as the coarse
full-step row: a model driving a firmware input cvar while the member mirrors 564
leaves; each world loads its own firmware copy; avg over 20 000 steps).

| phase                                       |     µs/step | ×realtime |
|---------------------------------------------|------------:|----------:|
| full step, firmware's own 1 ms interrupts    | 1.07–1.36 µs | 37–47×   |
| + a 50 µs interrupt (every step dispatches)  | 2.12–2.32 µs | 22–24×   |
| + a 50 µs interrupt, cadence off (`0`)       | 3.01–3.03 µs |   ~17×   |

**Result: the target is met** — a 50 µs world runs at ~22–47× realtime, comfortably
past the ≥10× (≤5 µs/step) bar. The gate is worth ~0.9 µs on every step that dispatches
without needing a mirror: what it removes is the whole-shadow `memcmp` traffic on the
steps that fall between mirrors (the decode+record of *changed* leaves is proportional
to changes, not to steps, so it is paid once per cadence either way).

**Fast set (not built).** A later stage needs a handful of ISR-written sample statics
mirrored at grid resolution while the rest of the namespace stays cadenced. That slots
in as a second range group built alongside the existing one in `build_shadow` and swept
*ahead of* the gate in `out_sync_cvars`; the gate is one `if` in front of a whole-set
sweep, so it neither shares nor constrains that group.

**The plant scales with sim time, not step count.** `motor.rs` derives its
integrator sub-step count from `dt_us` (1 µs sub-steps; a divisibility
`debug_assert` guards a future coarser integrator step), so the board world runs
on any grid and its physics cost is ~220 µs per millisecond of sim time
regardless of step size. A full fine-grid board world therefore lands near
~11 µs/step at 50 µs (≈4.5× realtime) — the plant is the floor, not the
framework; the fine-grid numbers above isolate the framework's own cost.

## 16. Stage-7 perf pass, phase 2a (2026-08-30)

Board world on the 50 µs grid, release, bridge dark, member-isolation rows
from the perf binary's `-- board-world report --` section (new):

| change                                     | µs/step | ×realtime |
|--------------------------------------------|--------:|----------:|
| baseline (1 µs integrator, string-key IO)  |    25.0 |      2.0× |
| motor integrator sub-step 1 → 5 µs         |    18.3 |      2.7× |
| `SigHandle` resolve-once port IO           |     6.8 |      7.3× |

- The §15 "plant is the floor" estimate is superseded: the plant's cost was
  ~85% avoidable (per-access `SignalId` construction + hashing — ~450k/sim-s
  — and 4/5 of the integrator iterations). Post-pass shares: firmware member
  3.0 (per-step Binding sync + fiber ISR), motor 2.4 (the ODE itself),
  encoders 0.67, sense 0.38, engine residual ~0.3.
- Integrator 5 µs is owner-decided, measured-identical: all physics tests and
  the north-star residuals hold at 5 and 10 µs (average-value bridge; L/R in
  the hundreds of µs).
- Models resolve `SigHandle`s at enable (`StateTable::handle` /
  `current_f64` / `record_by` — the public face of the index lanes routes
  already used). Member-side write short-circuiting was measured moot after
  this (0.3–0.4 µs/model total).
- Next lever: member-declared cadence / event-driven advance —
  [`member-cadence.md`](member-cadence.md).

## 17. Stage-7 perf pass, phase 2b: member-declared cadence (2026-08-30)

[`member-cadence.md`](member-cadence.md) landed: `Cadence` on the `Member`
trait (`EveryStep` default), encoders `OnDemand` (bus-driven — `transfer`
samples the table at the transaction instant, no scheduled advance, no
propagation pass), sense `OnInputChange` (post-epsilon input-dirty bits fed
by route propagation + scenario writes), and the firmware member's per-step
port-cache fill gated on its input-dirty bit. Same board-world row:

| change                                     | µs/step | ×realtime |
|--------------------------------------------|--------:|----------:|
| phase 2a exit (§16)                        |     6.8 |      7.3× |
| member-declared cadence (phase 2b)         |     5.7 |      8.8× |

- Post-pass shares (bridge dark): firmware member 2.64 (was 3.0 — the port
  fill gates off on quiet steps), motor 2.43 (unchanged — `EveryStep`, the
  real ODE), sense 0.23 (was 0.38), both encoders at measurement-noise level
  (was 0.67 combined; their two zero-latency propagation passes vanished with
  them).
- Measured-identical, as the design requires: north-star residuals exactly
  13.6 / 16.8 mA over 800 periods; full suites green in both cargo profiles.
- Cost now tracks events, not the grid: a spinning plant keeps sense + fw
  fill hot (currents change every step); idle worlds pay ~only motor + tick.
- Next lever: the engine-side next-event queue (skip empty grid steps
  outright) — `sim-interrupts.md` §5; cadence made "next event" well-defined.
