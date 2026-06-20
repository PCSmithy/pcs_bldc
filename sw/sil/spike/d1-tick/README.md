# D1 spike — cooperative fiber FreeRTOS port

Proves the load-bearing assumption of the whole SIL design (see
[`docs/sil/freertos-tick.md`](../../../../docs/sil/freertos-tick.md) §8 and
[`docs/sil/performance.md`](../../../../docs/sil/performance.md) Lever 1):
a **single-threaded cooperative fiber** FreeRTOS port is **deterministic** *and*
**many× faster than realtime**.

## What it is

A native (host) FreeRTOS V10.3.1 build with a hand-written **fiber port**:

- one OS thread; each task is a **Windows fiber**; the driver is the main fiber
- context switches are **fiber swaps** at cooperative points only
- the tick is **framework-driven** — the driver calls `vSilAdvanceTick()`
- quiescence (all tasks blocked → idle) hands control back to the driver via the
  idle hook (`vApplicationIdleHook` → `vPortYieldToScheduler`)

Two tasks exercise the scheduler: **A** (prio 3, 1 ms) and **B** (prio 2, 5 ms).

## Files

| File | Role |
|------|------|
| `port.c` / `portmacro.h` | the cooperative fiber port |
| `FreeRTOSConfig.h` | native config (cooperative, idle-hook handoff, timers off) |
| `spike.c` | driver + two tasks + idle hook; trace and bench modes |
| `build.sh` | build with MinGW gcc |

## Build & run

```bash
bash build.sh
./d1_spike.exe              # trace mode: per-tick task activity to stdout
./d1_spike.exe bench 50000000   # bench mode: ticks/s + x-realtime to stderr
```

## Results (MinGW gcc 15.2, one desktop)

**Determinism — PASS.** 30 serial + 16 parallel runs of trace mode are
**bit-identical**. Trace shows A every tick, B every 5th, A before B when both
are ready (priority order) — correct FreeRTOS scheduling, reproducibly.

**Performance — PASS.** ~**5.0 Mticks/s ≈ 5000× realtime** on one core at a
1 kHz tick — and this is a heavy case (A wakes *every* tick, so every tick does a
full switch in/out). 8 parallel sims hold ~4.3 Mticks/s each (~13% contention
loss) → near-linear scaling.

## What it proves / scope

- Validates the **fiber execution engine** (determinism + throughput) — the
  riskiest assumption — before anything is built on top.
- Raw scheduler-tick cost only; models, routes, historian, and real firmware
  come later and add per-tick cost. At a finer production base `dt` (a few µs),
  ~5 Mticks/s is ~10× realtime for the bare tick before that work.
- Windows fibers directly (Windows-first). The cross-platform context-switch
  primitive (macOS `ucontext` / small asm) is a later roadmap item.
- Throwaway scaffolding: the production port reuses this structure but lives in
  the canonical FreeRTOS portable layout.
