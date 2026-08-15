# D1 — FreeRTOS execution: cooperative fiber port + pluggable tick (resolved)

Resolves open decision **D1** from [`architecture.md`](architecture.md): how the
FreeRTOS scheduler runs on the host and how its tick relates to sim time.

**Decision:** run FreeRTOS as a **single-threaded cooperative fiber port** — every
task is a user-space fiber, the framework is the "main" context, all in **one OS
thread**. A pluggable **tick source** drives the systick: wall-clock-paced
(realtime) or framework-driven (deterministic/fast). This is the primary design,
**revised** from an earlier "retrofit the OS-thread host ports" plan — fibers are
the foundation of *both* determinism and performance
([`performance.md`](performance.md) Lever 1).

---

## 1. Why fibers, not OS-thread host ports

The OS-thread host ports (MSVC-MingW / POSIX) run each task as an OS thread and
were going to pair with a two-thread framework↔firmware handshake. Both are
performance traps:

- OS-thread context switch ≈ **1–5 µs**; cross-thread handshake signals ≈ **1
  µs/tick** — at 10⁵–10⁶ ticks/s that alone can be **slower than realtime**.
- User-space **fiber** swap ≈ **30–100 ns**, single thread, no signaling — ~50×
  cheaper, and strictly more deterministic (cooperative, defined switch points).

So we **write a fiber-based port** (our code) instead of vendoring a host port.

## 2. The execution model

- **One OS thread.** Framework = main context; each FreeRTOS task = a fiber with
  its own stack.
- **Port primitives:** `portYIELD` = swap to the next fiber; start-scheduler =
  swap to the first task; critical section / interrupt-mask = a cooperative flag
  that defers sim-interrupt dispatch (no real preemption needed — §4).
- **Advance** (the `sil_fw_advance_time` + `sil_fw_dispatch_isr` control ABI,
  [`ffi-boundary.md`](ffi-boundary.md)): the framework moves the hardware
  timebase, then hands over each handler due on the grid — the port's own systick
  among them. Dispatch swaps into the firmware (scheduler) context and runs ready
  tasks until the **idle fiber** runs (quiescence), which swaps back to the
  framework; the dispatch call returns. All fiber swaps — no threads, no signaling.

## 3. Pluggable tick source (realtime vs framework-driven)

The systick is one sim-interrupt source ([`sim-interrupts.md`](sim-interrupts.md));
two pacing impls share one loop:

- **Framework-driven (fast):** advance sim-time to the next event and dispatch —
  runs flat out.
- **Realtime:** pace the advance to wall-clock.

Only the pacing differs; the firmware path is identical, so a scenario debugged
in fast mode behaves the same in realtime.

**Pacing lives in the driver, not the port.** Because control is inverted — the
driver drives each step ([`ffi-boundary.md`](ffi-boundary.md)) — the
realtime/fast distinction is simply whether the caller sleeps between steps.
The firmware exposes only per-step primitives; there is no port-level
`portGetNextTick()` to swap. This is simpler than the original framing and is
what the working integration does.

## 4. Determinism — by construction, and why cooperative is enough

- Switches happen **only at defined points** (`portYIELD`, tick/ISR dispatch), so
  scheduling is reproducible — no arbitrary-instruction preemption, no OS-thread
  nondeterminism.
- **Cooperative is also faithful in sim-time:** firmware bursts are instantaneous
  in sim time ([`time-virtualization.md`](time-virtualization.md) / D9), so a
  task's work takes zero sim-time and **always reaches a yield before sim-time
  advances** to the next interrupt. ISRs therefore never need to preempt mid-task
  — the cooperative model loses no sim-time fidelity here.

## 5. The fidelity trade: ISR-vs-task races are not surfaced

Because interleavings are deterministic and ISRs fire only at yield points, a
**data race between an ISR and a task that depends on mid-instruction preemption
is *not* exposed** (real preemptive hardware could tear it; cooperative fibers
can't). This is a deliberate trade — determinism + performance over race-hunting,
which is the right call for control-logic SIL. Race-hunting belongs to static
analysis / sanitizers / targeted tests on the real target. A future
**preemption-fuzz** mode (inject ISR dispatch at varied points, off the
deterministic path) could probe these if ever needed.

> This supersedes the earlier "data races surface as nondeterminism — a feature"
> framing, which only held for the OS-thread model.

## 6. Overruns

A task that never yields would **hang the sim** (no preemption to break it). A
per-advance **watchdog** (wall-time / iteration-count budget without reaching
quiescence) flags it as a task overrun — the bug to surface.

## 7. Mechanism / portability

A minimal **context-switch primitive** under the port:

- **Windows:** native fibers (`ConvertThreadToFiber` / `CreateFiber` /
  `SwitchToFiber`).
- **macOS:** `ucontext` (`makecontext`/`swapcontext`) or a tiny hand-rolled asm
  switch.
- **Preferred:** one small cross-platform primitive (hand-rolled asm /
  minicoro-style, ~tens of lines) so Windows and macOS share one mechanism rather
  than diverging fibers-vs-ucontext.

## 8. The spike (now fiber-based — proves determinism *and* performance)

1. Stand up FreeRTOS V10.3.1 with a **fiber-based cooperative port** on native
   (we author `port.c` + `portmacro.h`; no vendor host port).
2. Two tasks at different priority + rate (1 ms / 5 ms `vTaskDelayUntil`),
   framework-driven tick, idle-fiber quiescence.
3. Drive N ticks from a small C driver; log per-tick which task ran.
4. **Pass criteria:** (a) per-tick logs **bit-identical** across many runs and a
   parallel launch; **(b)** measured throughput (ticks/s) confirms the
   many×-realtime thesis.

The fiber spike validates the load-bearing assumption for the whole sim —
determinism and performance together — before anything is built on top.

## 9. Open

- Exact context-switch primitive (hand-rolled asm vs platform fibers vs a small lib).
- Watchdog thresholds (§6).
- Preemption-fuzz mode (§5), a future off-path addition.
