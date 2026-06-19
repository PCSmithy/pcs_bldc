# D6 — Inverter fidelity + base time-step (resolved as a contract)

Resolves open decision **D6** from [`architecture.md`](architecture.md): the
inverter representation, the model↔firmware exchange, and the base `dt`.

**Decision:** **averaged-duty** inverter as the default; **switching-resolved**
as an optional swappable model for studies that need edges. Model↔firmware
contract is **abc** (per-leg normalized duty in, phase currents + rotor angle
out), routed via the sim HW-driver state — no timer-register modeling. Base
`dt` is set by **model numerical stability** (typically finer than the PWM
period); the control ISR fires on an integer multiple, and the model advances
one `dt` per tick.

> Resolved as a *contract/convention*. Two values finalize only when the
> firmware exists (§5): the exact PWM sim-driver state entry (route source) and
> the real control-ISR rate (sets base `dt`).

---

## 1. Inverter representation — the fork

| | Averaged-duty (default) | Switching-resolved (opt-in) |
|---|---|---|
| Model sees | cycle-average leg voltage `duty × Vbus` | actual high/low leg state at sub-PWM resolution |
| Base `dt` | PWM period (~50 µs) | sub-µs (resolves edges + dead-time) |
| Cost | 1× | ~50–500× |
| Captures | control-relevant dynamics | + ripple, dead-time, sensing detail |

**Averaged is the default** because FOC / estimator / mode-FSM logic — the
thing this SIL exists to test — operates on cycle-average voltages and
currents. Switching ripple isn't the object under test, and averaging is far
cheaper and numerically stable.

**Switching-resolved is a swappable model** (same model trait, finer `dt`) for
the cases that genuinely need edges: current-sensing-scheme validation,
dead-time compensation, ripple / audible-noise studies. Opt in per scenario.

## 2. Model↔firmware contract

- **Interface is abc** — 3 leg duties in; 3 phase currents + rotor angle out.
  Matches the hardware/firmware. The model may use dq internally; that's
  private to the model.
- **Duties via the PWM sim-driver state, normalized `[0,1]` per leg** — read by
  a route, **not** raw `CCR`/`ARR` counts. The (future) sim `HW_PWM` driver
  stores whatever the firmware commanded and exposes a normalized duty entry,
  so the model never models timer register math.
- **Vbus from the power model** via a route. Averaged inverter math: phase
  voltages = f(3 leg duties, Vbus), common-mode handled.
- **Back-path = "conversions are models"** ([`state-route-tables.md`](state-route-tables.md)):
  phase currents → current-sense model → ADC counts (route into fw ADC state);
  rotor angle → encoder model → SPI bytes (route into fw SPI-rx state).

```
 fw PWM state ──route──▶ inverter model ──▶ motor model ──▶ i_abc, θ
   (norm duty)            (avg V_abc)         (electrical       │
        ▲                  ▲ Vbus              + mechanical)    │
        │              power model                              ▼
   firmware                                  current-sense ──route──▶ fw ADC state
   computes duty                             encoder model ──route──▶ fw SPI-rx state
```

## 3. Base `dt` + integration

- **Base `dt` is driven by model numerical stability, not the PWM rate.** The
  motor electrical step (τ = L/R, often a few µs) is typically *finer* than the
  PWM/control period (~50 µs), so it sets the base grid. The control ISR and
  systick fire on **integer multiples** of the base `dt` (the D8 grid,
  [`sim-interrupts.md`](sim-interrupts.md) §5).
- **The model advances exactly one base `dt` per tick** — no sub-stepping in the
  common case, because the grid already *is* the model's step. The integration
  method (explicit Euler / semi-implicit / …) is **owned by the model**; the
  framework only calls `model.advance(dt)`. (If one model ever needs a step
  finer than the shared grid, it may sub-step internally — the exception.)
- **Most base ticks fire no interrupt** — they advance the model and propagate
  routes while the firmware stays parked; every Nth tick the control ISR fires.
  Cheap, since no-ISR ticks don't execute firmware.
- **Switching-resolved** needs sub-µs steps anyway (to resolve edges +
  dead-time), so there the grid is set by switching detail rather than model τ.

## 4. What averaged deliberately abstracts away

Visible only in switching-resolved, so we know the trade:

- dead-time voltage error (duty-dependent, worst near current zero-crossings)
- intra-period ADC sampling phase (where in the PWM cycle currents are sampled)
- current ripple
- switch-state-dependent low-side-shunt sensing

For control-logic SIL these are correctly abstracted: the sensor model provides
the *true* (averaged) phase current the controller would compute on.

## 5. Finalizes when the firmware lands

The shape above is decidable now; these two numbers close later:

- the exact PWM sim-driver **state entry name** the input route reads from
  (depends on the future `HW_PWM` module), and
- the real **control-ISR rate**, which sets the base `dt`.

(Whether `HW_PWM`'s API takes normalized duty or counts doesn't matter to the
contract — the sim driver normalizes either way.)

## 6. Out of D6 scope

- **Motor BEMF shape** (trapezoidal "true BLDC" vs sinusoidal PMSM) is a
  Phase-3 *motor-model* design choice, not part of this contract.

## 7. Couplings

- **D8** — base `dt` = the interrupt grid.
- **State/Route** — the back-path conversions are models, routed.
- **D9** — Vbus / power model timing.
- **D7** — the model integrator is where float-determinism choices bite.
