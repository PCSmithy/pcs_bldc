# D9 — Firmware time virtualization (resolved design)

Resolves open decision **D9** from [`architecture.md`](architecture.md): which
firmware time sources are allowed, and how each is backed by sim time. The
FreeRTOS tick is already handled by D1; this covers `HAL_GetTick`, the DWT
cycle counter, free-running timer counters, and busy-wait delays.

**Decision:** sim time advances **only at yields/quiescence** — a firmware
execution burst is instantaneous in sim time. Therefore **every wait must be a
yield** and **every time read reflects the sim clock**. Time *reads* return
sim-clock-derived values (frozen within a burst); blocking *delays* are
converted to cooperative sim-time advances; portable firmware gets time only
via FreeRTOS delays or a `HW_time` abstraction (both sim-backed).

---

## 1. The hazard: busy-waits deadlock

On hardware, counters (SysTick, DWT, timers) advance autonomously regardless of
what the CPU runs. In sim, **the framework advances the clock only when the
firmware reaches quiescence** (D1). So a wait that does **not** yield deadlocks:

```c
HAL_Delay(10);                  // spins until uwTick advances 10
while (HAL_GetTick() < t) { }   // spins until the tick counter reaches t
delay_us_via_DWT(50);           // spins until CYCCNT advances
```

The firmware thread spins without yielding → never reaches quiescence → the
framework never advances sim time → the counter never moves → **infinite spin.**
By contrast `vTaskDelay()` yields: firmware goes idle → sim advances → the tick
wakes it. The rule that falls out:

> **Every wait is a yield; every time read reflects the sim clock.**

## 2. Time reads → sim-clock-derived (frozen within a burst)

Sim HW-layer shims return values computed from the framework's current
sim-time:

| Source | Sim value |
|--------|-----------|
| `HAL_GetTick()` | sim-time in ms |
| `DWT->CYCCNT` | sim-time × f_cpu |
| timer `CNT` (`__HAL_TIM_GET_COUNTER`) | sim-time × f_timer, mod ARR |

These are **frozen within a single burst** (sim time doesn't move mid-burst),
which is consistent with reality: a real burst is microseconds, below the 1 ms
tick resolution. Cross-burst (after a yield) they advance correctly. The
FreeRTOS tick and `HAL_GetTick` read the same sim clock, so they stay
consistent.

## 3. Blocking delays → cooperative sim-time advances

The sim layer's delay implementations don't spin — they **yield and advance**:
block the firmware thread, signal the framework to advance sim by the requested
interval (firing any due interrupts along the way, D8), then resume. Semantically
a sim-time sleep:

- `HAL_Delay(ms)` → advance sim by `ms`.
- `delay_us(n)` → advance sim by `n` µs.

Mostly relevant for pre-scheduler bring-up and short driver-level waits, where
there are no other tasks to preempt and this is exactly faithful.

## 4. Policy: how portable firmware gets time

- Portable **app / io / dev** code uses **FreeRTOS delays** or a **`HW_time`**
  abstraction (a new HW module, or an extension of `HW_systemClock`) with
  stm32g4 + sim impls — never `HAL_Delay` / `DWT` / timer-`CNT` directly.
- The stm32g4 `HW_time` impl uses SysTick/DWT/timer; the sim impl reads the sim
  clock (§2) and converts delays to yields (§3).
- **Audit** the existing IO drivers (`IO_AS5048`, `IO_SK6805`, …) for direct
  time-source use when they're ungated for SIM (Phase 1), and route any through
  `HW_time`.
- On native there is **no vendor HAL** (it's embedded-only), so the sim HW layer
  is already the bottom — there are no HAL-internal busy-waits beneath us to
  worry about.

## 5. Limitations (documented, by design)

- **Self-profiling is meaningless in SIL.** Measuring your own execution time
  (read DWT before/after a code block) reads ~0 elapsed — sim time is frozen
  during the burst and host wall-time is irrelevant. SIL tests timing via the
  sim clock and the interrupt schedule, not self-measurement.
- **Busy-delays inside a running task aren't faithfully preemptible.** A
  spin-delay in a task that an ISR-readied higher-priority task should preempt
  is not modeled — but that's bad RTOS practice anyway. The rule is "use
  `vTaskDelay`," not "we emulate it."

## 6. Couplings

- **D1** — the FreeRTOS tick; same yield/quiescence model.
- **D8** — delays advance sim time and fire due interrupts through the same
  mechanism.
- **D6** — sim-clock units / base `dt`.
- **Phase-1 firmware-on-SIM** — the `HW_time` module + the driver audit.
