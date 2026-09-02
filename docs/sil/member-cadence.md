# Member-declared cadence — event-driven advance

Drafted 2026-08-30 (stage-7 perf pass, phase 2b); revised same day with the
owner-approved `OnDemand` variant + transfer-context contract, and
**implemented** (numbers: `performance.md` §17). Owner-motivated:
this is a **long-term correctness** item for voyant's shape, not only a perf
lever — a member should declare how it advances; the engine grid is an
implementation detail, not part of any device's identity.

## Problem

The fixed base-`dt` engine advances **every member every step**. On the 50 µs
control-rate grid that means encoders "compute" at 20 kHz against real devices
that frame in the kHz range, the sense model re-evaluates an unchanged affine
transform, and the firmware member re-syncs its port bindings whether or not
anything moved. Post-phase-2a shares (performance.md §16): firmware member
3.0 µs/step, motor 2.4, encoders 0.67, sense 0.38 of a 6.8 µs step.

## Decision sketch

`Member` gains a cadence declaration; the engine advances only due members.

```rust
enum Cadence {
    /// Advance every engine step (default — today's behavior, zero migration).
    /// For members that integrate continuously: the motor.
    EveryStep,
    /// Advance when `period_us` of sim time has elapsed since the last
    /// advance — drift-free absolute due times, the IRQ-table discipline.
    /// For framed devices with internal update pipelines worth modeling.
    Periodic { period_us: u64 },
    /// Advance only on a step where a route delivered a change into one of
    /// this member's registered inputs. For pure transforms: the sense model.
    OnInputChange,
    /// Never scheduled — the bus drives it. All behavior lives in
    /// `DuplexPeer::transfer`, which samples the member's routed inputs from
    /// the table at the transaction instant. For memoryless bus responders:
    /// the encoders.
    OnDemand,
}
```

- **`dt_us` semantics change with it**: `advance(dt_us, …)` receives the sim
  time elapsed since the member's *previous* advance (exactly the grid step
  for `EveryStep`, preserving today's behavior). The motor's `dt`-derived
  sub-step count already handles a variable `dt`; transform members ignore it.
  Re-enable resets the baseline to now — a disabled gap is frozen time, never
  integrated through.
- **Input-dirty plumbing**: route propagation reports each destination it
  actually **changed** (post-epsilon — an unchanged redelivery is not an
  event). The engine maps destination source-name → member (members own their
  `vsig:<name>:…` namespace) and sets a per-member `inputs_dirty` bit; a
  scenario `Engine::write` marks its target's member the same way.
  `OnInputChange` consumes the bit as its due test; every advance clears it
  and receives it via `MemberCtx`. The table stays dumb — the engine owns the
  mapping. **Dirty at birth** (and at re-enable): the first scheduled advance
  always runs, so an `OnInputChange` member publishes its resting outputs —
  a sense chain's bias volts — even in a world where nothing ever drives it
  (the fault-injection worlds suspend its input routes from step one).
- **Determinism**: due times are absolute sim-µs from the declaration; among
  due members the existing registration order holds. No behavior depends on
  wall clock or on how many steps a quiet interval was divided into.
- **The firmware member stays `EveryStep`** — `advance_time` must run each
  step for TIM landings/trigger emission. Its own reduction is the fw-side
  twin of `OnInputChange`: gate the per-step port-cache fill (unconditional
  since the zero-latency reorder) on `MemberCtx`'s `inputs_dirty` (or pending
  port registrations); the cvar flush is already sparse
  (`take_dirty_indices`) and
  `out_sync` already gates on dispatch.

## Transfer-driven members (`OnDemand`) — the bus is the clock

Owner-decided: the encoder needs no periodic advance at all. Its `advance`
was only an input pump (latch the routed angle, publish trace outputs) forced
by `DuplexPeer::transfer` having no table access. Give `transfer` the same
[`MemberCtx`] an `advance` gets and the pump disappears: the peer samples its
routed inputs **at the transaction instant** — what the physical device does —
and records its own outputs there too.

- **Mid-step reads are the truthful semantic.** A transfer fires inside the
  initiator's advance and sees the table "as of now": members earlier in
  registration order have advanced, later ones haven't. That is what a real
  SPI exchange samples. Producer→sensor→firmware ordering makes it exact.
- **Contract (same as `advance`, now stated for peers):** a peer touches only
  its own signals — reads its routed inputs, writes its registered outputs.
  Resolve-once handles make this near-structural. Writes follow the table's
  one-shot last-writer-wins semantic; cross-member coupling stays in routes.
- **Stateful on-demand devices catch up lazily.** A transfer-driven model
  *with* internal dynamics advances its own state to now before answering —
  the same elapsed-time primitive the cadence bookkeeping uses. A model may
  combine roles (cadenced background dynamics + transfer-time catch-up) when
  a device warrants it. The encoders are memoryless; they need none of this.
- **Mechanism, model-initiated path:** `MemberCtx::duplex_transfer` hands its
  own table borrow down through the router, which builds the peer's ctx —
  safe Rust end-to-end; nested (bridge) transfers recurse the same way, and
  the self-transfer cycle still panics loud.
- **Mechanism, firmware-initiated path:** the SPI upcall crosses a C stack
  frame, which cannot carry a Rust borrow. The dispatching member stashes a
  raw table pointer in the router strictly around `dispatch_isr` — the same
  single-threaded raw-context discipline every trampoline in `backend.rs`
  already documents. No stash (e.g. firmware `start`) = floating bus, as
  today.
- **Encoders: `OnDemand` replaces the earlier Periodic-at-datasheet-rate
  idea.** A slow frame cadence would *introduce* staleness real hardware
  doesn't have; on-demand sampling keeps value truth exact with zero
  scheduled cost. `raw_encoder_ticks` records at read time (the firmware
  polls every tick, so trace density is unchanged); the internal CORDIC
  pipeline stays unmodeled until physics evidence wants it, and `Periodic`
  is its natural home then.

## Relation to the event-driven timeline (sim-interrupts.md §5)

Cadence is the **member-side half**; the engine-side next-event queue
(skipping empty grid steps outright) is the complement and stays the future
upgrade. Cadence makes the queue effective later — "next event" becomes
min(member due times, IRQ due times) — and is independently useful now.
Implement cadence first; it is contained and default-compatible.

## Expected outcome and guardrails

The shape gain is the point: cost now tracks *events* (SPI polls, input
changes), not the grid. Concretely: the encoders' scheduled cost drops to
zero (their transfer work tracks the firmware's poll rate — unchanged while
it polls every tick, free when it doesn't); their propagation passes vanish
from the M×R seam; sense + fw port fill gate off on quiet steps (idle
scenarios most; spinning scenarios keep those hot since currents change
every step). Guardrails before landing:

- North-star residuals hold **exactly** (13.6/16.8 mA at the quantization
  floor) — transfer-time sampling reads the same same-step value the advance
  latch held, so this is measured-identical, not approximately-equal.
- Full suites green in both cargo profiles; `EveryStep` default means every
  existing world is behavior-identical until a model opts in.
- Perf-binary board rows re-measured and appended to performance.md.

## Out of scope here

The engine next-event queue; route-latency semantics (settled — zero-latency
delivery landed with the `in_sync` reorder); plant model internals (the
integrator's 5 µs sub-step is an owner-decided constant, revisited only with
physics evidence).
