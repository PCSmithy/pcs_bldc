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
the in-sync **flush is sparse** (only the command-dirtied + pinned cvars, via the
State Table dirty set, never the whole namespace). Measured on the pcs_bldc DLL:
**~430 cvar leaves** swept per tick (the built-in array-size exclusion drops the
task stacks / heap / 512-byte buffers that would otherwise dominate). The sweep
itself is a small fraction of a firmware advance today (the FreeRTOS tick + task
switches dominate the measured µs/advance); dirty-page tracking is the lever when
the naive scan profiles hot.

## 6. Lever 5 — zero-alloc hot loop

No `malloc` per tick. Preallocate everything; historian append = bump-pointer
into **chunked per-signal columnar buffers**; routes as flat `(src, dst, size)`
arrays iterated with `memcpy`. Steady-state allocation-free. Easy to design in,
miserable to retrofit.

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
