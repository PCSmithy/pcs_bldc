# D7 — Determinism & float tolerance (resolved design)

Resolves open decision **D7** from [`architecture.md`](architecture.md): how
regression baselines handle floating-point, and what determinism the engine
guarantees.

**Decision:** all float *value* assertions are **tolerance-based** (default
ε ≈ `1e-6`, per-signal override) — there is **no bit-exact contract**.
Separately, the engine stays **deterministic / reproducible** run-to-run within
a platform (cheap, mostly already free), which keeps tolerance tests flake-free
and discrete outcomes stable. Native float ≠ embedded float, so SIL is
**behavioral-within-tolerance vs hardware, never exact**.

---

## 1. Two different things — don't conflate them

| | What it is | Our choice |
|---|---|---|
| **Assertion style** | how a test checks a value | **tolerance (ε)** — always |
| **Reproducibility** | same inputs → identical run | **kept** — a property of the engine |

A run can be perfectly reproducible *and* still be asserted with tolerance.
We assert with ε; we keep reproducibility because it's what makes ε-tests
reliable (§3–4).

## 2. Tolerance assertions

- **Default ε ≈ `1e-6`** for float signals; **per-signal override** where a
  signal needs looser/tighter.
- Applies the same way **within** and **across** platforms — no special
  bit-exact mode, no per-platform golden baselines for float values.
- **Bounded below by the trace deadband.** The historian stores float signals
  with an ε-deadband (default `1e-3`, [`signal-trace.md`](signal-trace.md) §5),
  so a signal's trace can't support an assertion tighter than its deadband:
  **assert-tolerance ≥ signal-deadband.** To assert tightly, tighten that
  signal's deadband too.

## 3. The engine stays reproducible (cheap — mostly already done)

- **Deterministic scheduling** — already resolved: the D1 tick, D8 interrupt
  ordering (priority + registration-index tiebreak), and the route
  snapshot-then-write pass ([`state-route-tables.md`](state-route-tables.md) §3).
- **One seeded PRNG** for everything stochastic (sensor noise, fault timing,
  jitter): seed set per scenario, **recorded in the trace**, fully replayable.
- **No entropy in models** — no wall-clock reads, no unseeded random, no
  dependence on host thread ordering (the firmware is serialized by the port +
  the D1 handshake).

Result: run-to-run variance within a platform is ≈ 0, so tolerance tests don't
flake and reruns reproduce failures exactly.

## 4. The real gotcha: discrete divergence

Floats that feed **branch decisions** diverge **categorically**, not within ε:

```c
if (current > limit)   // a 1e-9 difference can flip this...
```

A flipped branch doesn't yield a "1e-6-off" value — it sends the FSM down a
different path, so a *discrete* outcome (a state, an event count, pass/fail)
is simply different. Therefore:

- **Within a platform:** §3 reproducibility keeps these stable run-to-run.
- **Cross-platform / vs hardware:** a test sitting on a knife-edge threshold can
  flip. **Mitigation is test design, not tighter ε** — exercise discrete
  outcomes with **margin off thresholds**, don't assert on borderline inputs.

## 5. vs hardware

The native host FPU + libm differ from the MCU's single-precision
`fpv4-sp-d16` (and its libm). So SIL results **never bit-match the real MCU** —
SIL validates control *behavior* within tolerance, not hardware-exact numbers.
This is inherent to the native-execution choice ([`ffi-boundary.md`](ffi-boundary.md));
an ARM-emulation backend would be the only way to close it, if ever needed.

## 6. Couplings

- **D6** — the model integrator is the main float producer; deterministic given
  seed + ordering.
- **D1 / D8** — deterministic scheduling (already resolved).
- **D2** — native execution is the source of the vs-hardware float gap.
- **D12** — the trace stores the seed (+ any per-signal tolerances).
- **D10** — scenarios set the seed and tolerance overrides.
