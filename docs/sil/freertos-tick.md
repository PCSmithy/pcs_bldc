# D1 — FreeRTOS tick source (resolved design)

Resolves open decision **D1** from [`architecture.md`](architecture.md): how
the FreeRTOS scheduler tick relates to sim time.

**Decision:** a **pluggable tick source** at the FreeRTOS port layer, with
two implementations — *realtime* (wall-clock paced) and *framework-driven*
(deterministic). Realtime and fast mode become the **same control loop** with
only the pacing swapped. Implementation approach: **retrofit the existing
host ports first**; escalate to a custom cooperative port only if empirical
determinism proves insufficient (see §6).

---

## 1. The lever: how host ports generate ticks

Both FreeRTOS host-simulator ports we need —

- **MSVC-MingW** (Windows; works with the project's MinGW gcc), and
- **POSIX / GCC** (macOS) —

already **serialize task execution**: each task is an OS thread, but only one
is runnable at a time; the scheduler signals the next thread and blocks the
current one. The tick is produced by a *separate* generator — a timer thread
that sleeps one tick period then injects a simulated tick (MSVC), or a
`SIGALRM` timer (POSIX).

That generator is the seam. Because FreeRTOS itself deterministically chooses
*which* task runs given a tick, **controlling *when* ticks fire is sufficient
to control scheduling.** We replace "sleep, then tick" with a pluggable
`portGetNextTick()`:

```c
// Port-layer seam. One of two impls is selected at sim start.
void portGetNextTick(void);   // returns when the next tick should fire
```

- **Realtime impl:** sleep until `start + n * tickPeriod` (wall-clock pacing).
- **Framework impl:** block until the framework signals "advance"; no internal
  clock at all.

## 2. Control flow: two threads + a quiescence handshake

Control is inverted so the framework owns the outer loop, instead of
`vTaskStartScheduler()` blocking forever.

- **Firmware thread** — runs the real host-port scheduler and the real tasks.
- **Framework thread** (Rust) — owns the sim clock, plant models, signal log.
- They rendezvous twice per tick: framework→fw "advance one tick", fw→
  framework "quiescent".

**Quiescence = the idle task got control** (every real task is blocked on a
delay / queue / notification). `vApplicationIdleHook` is the detector: when
idle runs, it posts "quiescent" to the framework and blocks the firmware
thread until the next "advance".

One tick:

```
framework:  models.advance(dt)               // plant forward one base step
            push_sensors -> sim HW drivers    // ADC counts, encoder SPI, GPIO
            signal fw "advance"; wait "quiescent"
firmware:     xTaskIncrementTick();           // the controlled tick
              run ready tasks -> ... -> idle hook fires
            pull_actuators <- sim HW drivers   // commanded PWM, LED, SPI tx
            record_signals()
            tick_source.wait_next()            // realtime: sleep · fast: now
```

**The only line that differs between modes is the last one.** Same loop, same
scheduling, same firmware code path. A scenario debugged in fast mode behaves
identically in realtime (modulo pacing) — that is the entire payoff of the
abstraction.

> Sensor data is *pushed by the framework* into the sim HW drivers before the
> tasks run (replacing the drivers' current synthetic ramp), and actuator
> commands are *pulled* after. Within one tick all tasks see one consistent
> sensor snapshot, which is physically reasonable at a fine base `dt`.

## 3. Determinism — guarantees and limits

- **Guaranteed at tick granularity:** which task runs at each tick boundary,
  round-robin order among equal priorities, and higher-preempts-lower — all
  reproducible, because the tick is the only async event and the framework
  owns it.
- **Not controlled:** the exact instruction where a preemption lands *within*
  a task. This matters only when (a) a task overruns its period (a tick fires
  mid-run) or (b) tasks share memory without proper synchronization.
- **Case (b) is a feature:** if the firmware has a data race, SIL surfacing it
  as nondeterminism is the desired behavior — we do not hide it.
- **Run-to-quiescence is the default:** well-behaved firmware (tasks finish
  inside their period) reaches idle before every tick, so there is never a
  mid-run preemption → **full determinism for free.** Overruns fall back to
  the host port's faithful (but not instruction-deterministic) preemption,
  and the overrun itself is usually the bug worth catching.

## 4. Generalization: systick is one simulated interrupt among several

The fast control loop on hardware almost certainly runs off a timer / ADC-
complete **ISR** at the PWM rate (tens of kHz), not a 1 ms FreeRTOS task. So
the tick source is really the first instance of a **simulated interrupt
scheduler**:

- The framework base `dt` is the **fastest** source (the control ISR).
- The FreeRTOS systick (e.g. 1 ms) is one periodic event on that timeline.
- Other ISRs (control timer, ADC EOC, …) register as additional sources.

D1 delivers the **systick source** first, but the seam is designed as
"register a periodic sim interrupt source" so the control ISR and friends slot
into the same mechanism. That mechanism is now specified as **D8** — see
[`sim-interrupts.md`](sim-interrupts.md). This ties into D6 (base dt / inverter
sampling).

## 5. Portability

Two port files, one shared tick-source interface:

- **Windows:** retrofit `portable/MSVC-MingW/port.c` — replace the
  `prvSimulatedPeripheralTimer` thread's sleep+tick with `portGetNextTick()`.
  Event-based, cleanest to retrofit.
- **macOS:** retrofit `portable/ThirdParty/GCC/Posix/port.c` — replace the
  `SIGALRM` tick timer with the same seam. Needs care with signal masks /
  pthread interaction.

Both must expose identical `portGetNextTick()` + quiescence-hook semantics so
the framework loop is platform-agnostic.

## 6. The spike (do this before building anything on top)

Highest-risk item in the whole SIL effort. Prove it in isolation:

1. Stand up a host-port FreeRTOS build with two tasks at different priorities
   and rates (e.g. 1 ms + 5 ms `vTaskDelayUntil` loops) on the **native**
   target.
2. Wire the framework-driven tick source + the idle-hook quiescence handshake.
3. Drive N ticks from a trivial Rust (or C) harness; log per-tick which task
   ran.
4. **Pass criterion:** run the same scenario many times (and across a parallel
   `pytest -n`-style launch) and the per-tick logs are **bit-identical**.
5. Repeat the retrofit + spike on the macOS POSIX port.

**If tick-granularity determinism holds → done, ship the host-port retrofit.**
If it doesn't (unexpected mid-tick nondeterminism that isn't a firmware race),
escalate to a **custom cooperative single-threaded port** (setjmp/longjmp or
ucontext coroutines, context switches only at defined yield points) for
instruction-level determinism — more work and a port we maintain, hence the
fallback rather than the default.

## 7. Open implementation choices (not blocking the spike)

- Exact handshake primitive (events vs condition vars vs semaphores) per port.
- Where the framework↔firmware boundary sits relative to the FFI (D2): the
  "advance/quiescent" signals likely ride the same in-process FFI seam.
- Whether realtime mode paces in the tick source (sleep) or in the framework
  loop — equivalent; pick by where the wall-clock reference lives.
- Tick rate vs base `dt` reconciliation once the control ISR source lands (D6).
