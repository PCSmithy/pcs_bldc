# Member-declared cadence — event-driven advance (design, pre-implementation)

Drafted 2026-08-30 (stage-7 perf pass, phase 2b). Owner-motivated: this is a
**long-term correctness** item for voyant's shape, not only a perf lever — a
member should declare how it advances; the engine grid is an implementation
detail, not part of any device's identity.

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
    /// For framed devices: encoders at their datasheet frame rate.
    Periodic { period_us: u64 },
    /// Advance only on a step where a route delivered a change into one of
    /// this member's registered inputs. For pure transforms: the sense model.
    OnInputChange,
}
```

- **`dt_us` semantics change with it**: `advance(dt_us, …)` receives the sim
  time elapsed since the member's *previous* advance (equal to the grid step
  for `EveryStep`). The motor's `dt`-derived sub-step count already handles a
  variable `dt`; framed/transform members ignore it.
- **Input-dirty plumbing**: `record` already maintains a dirty set; route
  propagation knows which destinations changed. Map destination source-name →
  member (members already own their `vsig:<name>:…` namespace) and set a
  per-member `inputs_dirty` bit during propagation; `OnInputChange` consumes
  and clears it. The table stays dumb — the engine owns the mapping.
- **Determinism**: due times are absolute sim-µs from the declaration; among
  due members the existing registration order holds. No behavior depends on
  wall clock or on how many steps a quiet interval was divided into.
- **Duplex is unaffected** (and is the physical justification): SPI transfers
  are firmware-pulled through `DuplexPeer::transfer`, not member-advance-
  driven. An encoder between advances answers with its last **latched** frame
  — exactly what the real AS5048 does between CORDIC updates. Frame period
  becomes per-instance config (`with_cadence(us)`), taken from the datasheet.
- **The firmware member stays `EveryStep`** — `advance_time` must run each
  step for TIM landings/trigger emission. Its own reduction is the fw-side
  twin of `OnInputChange`: gate the port `in_sync` (unconditional since the
  zero-latency reorder) on the member's `inputs_dirty` bit; `out_sync`
  already gates on dispatch.

## Relation to the event-driven timeline (sim-interrupts.md §5)

Cadence is the **member-side half**; the engine-side next-event queue
(skipping empty grid steps outright) is the complement and stays the future
upgrade. Cadence makes the queue effective later — "next event" becomes
min(member due times, IRQ due times) — and is independently useful now.
Implement cadence first; it is contained and default-compatible.

## Expected outcome and guardrails

Encoders + dial + idle sense drop to ~0 between their due steps and the fw
`in_sync` gates off on quiet steps: the board world lands in the ~10–15×
realtime range (from 7.3×). Guardrails before landing:

- North-star residuals hold (13.6/16.8 mA at the quantization floor) with
  encoders on datasheet cadence — commutation reads latched SPI state, so
  the angle a control ISR sees must stay within one frame period of truth.
- Full suites green in both cargo profiles; `EveryStep` default means every
  existing world is behavior-identical until a model opts in.
- Perf-binary board rows re-measured and appended to performance.md.

## Out of scope here

The engine next-event queue; route-latency semantics (settled — zero-latency
delivery landed with the `in_sync` reorder); plant model internals (the
integrator's 5 µs sub-step is an owner-decided constant, revisited only with
physics evidence).
