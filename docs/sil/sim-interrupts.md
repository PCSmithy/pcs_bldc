# D8 — Simulated interrupt model (resolved design)

Resolves open decision **D8** from [`architecture.md`](architecture.md): how
non-systick interrupts (control-loop timer, ADC-EOC, SPI/UART/DMA-complete,
EXTI, …) fire in sim time. Generalizes the D1 tick source into a framework-
owned **interrupt controller**.

**Decision:** the framework owns an **interrupt table** of handler entries.
Sources are **periodic or one-shot**, registered either **at config time**
(framework-side, by handler name) or **at runtime** (by the sim HW-layer code,
by handler pointer, via a C→Rust upcall). The framework schedules; the firmware-
side **port dispatches** each due handler in the firmware fiber context so FreeRTOS
`...FromISR` semantics hold. Fixed base-`dt` grid, priority-ordered, no nesting.

---

## 1. The interrupt table

A framework-owned table of entries. One structure serves all four registration
paths (config/runtime × periodic/one-shot):

```
entry = {
  handler:       fn pointer (into the firmware image)
  kind:          Periodic | OneShot
  rate_or_delay: sim-time literal   // period (Periodic) or delay-from-now (OneShot)
  priority:      u8                  // ordering only (no preemption — §6)
  enabled:       bool
}
```

Registration returns a **handle**; `cancel(handle)` / `disable(handle)` removes
or masks it (a periodic timer the firmware stops must be removable).

## 2. Registration paths

| Path | Who | Handler given as | Example |
|------|-----|------------------|---------|
| **Config, periodic** | framework / scenario | handler **name** → resolved to a pointer via DWARF/dlsym | systick, control-loop timer |
| **Runtime, periodic** | sim HW-layer driver | handler **pointer** | a timer the firmware starts at runtime |
| **Runtime, one-shot** | sim HW-layer driver | handler **pointer** | SPI/UART/DMA-complete after a transfer |
| (Config one-shot) | framework / scenario | name | rare; e.g. a scripted fault at T |

- **Handlers are plain function pointers** — *not* restricted to CMSIS vector
  names. CMSIS names (`TIM1_UP_IRQHandler`, `ADC1_2_IRQHandler`) are just the
  common config-time case, resolved by name; runtime registration passes the
  pointer directly (a vector handler, a HAL callback, or a sim function).
- **Rate/delay is a hardcoded literal.** The API does **no** prescaler/clock
  arithmetic. If a caller wants a rate derived from peripheral config, it
  computes the number itself and passes it into the generic `rate_or_delay`
  field. The table stays dumb.

## 3. Who registers at runtime — the sim-HW-layer convention

Runtime registration is a **C→Rust upcall**, and it is called **only from
sim-target HW-layer code** (`sw/lib/c/shared/hw/<X>/sim/` drivers + sim port
glue) — never from portable app/io/dev firmware. That code is sim-only by
definition, so it is the legitimate home for sim-awareness, and D2's "portable
firmware is sim-unaware" principle holds intact:

```
IO_AS5048            calls HW_SPI_transmit(...)          // portable, sim-unaware
  └─ sim HW_SPI      calls sil_irq_register_oneshot(&SPI3_IRQHandler, 2_us, prio)
       └─ framework  schedules the SPI-complete interrupt; dispatches it in 2 us
```

This adds a **C→Rust upcall direction** to the FFI (D2 was Rust→C only). It is
**control, not data**, so it does not reintroduce the getter/setter pattern we
removed. Mechanics: Rust hands C a struct of `extern "C"` callback pointers at
init; the sim drivers call through them. See
[`ffi-boundary.md`](ffi-boundary.md) "C→Rust upcalls".

## 4. Dispatch context

The framework owns *scheduling* (when a source is due); the firmware-side
**port owns dispatch** (the context the handler runs in). At a due time the
framework, having swapped into the firmware (D1 fibers), runs pending handlers
**in the firmware context** through a thin shim that brackets each call with the
port's ISR entry/exit. This makes `x...FromISR` wakeups and `portYIELD_FROM_ISR`
behave exactly as on hardware — a handler can unblock a high-priority task and
that task (its fiber) runs before the firmware returns to quiescence. (Bare-
calling the handler outside the port's ISR bracket would break its bookkeeping.)

Handler-mode / MSP / privilege are **not** modeled — handlers are ordinary C
functions. We don't model memory protection, so this is a non-issue.

## 5. Timeline — fixed base-`dt` grid

- **Base `dt` = the finest step needed = max(fastest-ISR rate, model-stability
  rate).** In practice the **model-stability step usually dominates** (a few µs,
  finer than the ~50 µs PWM period), so it sets the grid and the control ISR /
  systick fire on **integer multiples**. (Ties to D6,
  [`inverter-timestep.md`](inverter-timestep.md) §3.)
- **One model advance per tick** in the common case (grid = model step); most
  base ticks fire no interrupt and just advance the model + routes with the
  firmware parked. A model stiffer than the shared grid may sub-step internally
  — the exception.
- **One-shots are quantized to the grid** (fire at the next step boundary); a
  fine base `dt` keeps that quantization tight.
- **Event-driven timeline** (a next-fire-time queue, exact aperiodic latency,
  variable model step) is the future upgrade if grid quantization of aperiodic
  interrupts ever distorts timing that matters. Fixed-grid is the start.

## 6. Masking, enable, priority, nesting

- **Masking + pending:** `__disable_irq` / `portENTER_CRITICAL` must hold a due
  interrupt **pending**, not drop it. The controller keeps a pending state and
  honors a simulated interrupt-enable/mask held in the fiber port (a cooperative
  flag — D1; no real preemption to gate).
- **Per-IRQ enable** is modeled (native has no NVIC, so `HAL_NVIC_EnableIRQ`
  would otherwise be a no-op, and firmware relies on "disabled until
  configured"). `enabled` on the entry + register/cancel covers it.
- **Priority is ordering only.** When several are due at the same step, run them
  in priority order; **no nesting / no ISR-preempts-ISR** — each handler runs to
  completion. Document the limitation; revisit only if firmware needs it.

## 7. Per-tick ordering (data timing)

Interrupts slot into the State/Route loop's "FW step"
([`state-route-tables.md`](state-route-tables.md) §3): inputs are propagated
into firmware statics **before** the step, so a control ISR dispatched in the
step reads *this* step's ADC values; outputs propagate **after**. Per base
tick:

```
1. advance models (dt; may sub-step)
2. propagate routes (snapshot sources → write dests) — fresh inputs into fw statics
3. FW step: dispatch all interrupts due at this sim-time (priority order),
            running tasks to quiescence between/after them
4. record; asserts/injection
5. pace (realtime: sleep · fast: now)
```

## 8. Determinism

Every interrupt is framework-scheduled, so timing is reproducible. When
multiple fire at the same step: **priority order, then registration-index
tiebreak** — fully deterministic (D7). One-shots registered during a step are
scheduled relative to the current sim-time, also deterministic.

## 9. Concurrency safety

Single thread (D1 fibers): runtime registration upcalls are plain synchronous
calls from the firmware fiber *during* a step, while the framework's main context
is swapped out. The interrupt table is mutated only mid-step (firmware) and read
only between steps (framework) — never concurrently. The same single-thread
property that protects firmware memory ([`ffi-boundary.md`](ffi-boundary.md) §5)
protects the table. No locking.

## 10. Open / future

- Event-driven timeline (§5) if aperiodic latency fidelity is needed.
- Deriving periodic rates from firmware timer config (vs hardcoded literals) —
  explicitly *not* in the registration API; a caller may compute and pass it.
  Ties to D9.
- ISR nesting/preemption (§6) if a real firmware case demands it.
