# D12 — Signal trace / State Table historian (resolved design)

Resolves open decision **D12** from [`architecture.md`](architecture.md): what
gets recorded, at what rate, and in what form. The answer is that the **State
Table is the historian** — there is no separate trace subsystem.

**Decision:** every traced State Table entry
([`state-route-tables.md`](state-route-tables.md)) accumulates a **change-logged,
timestamped time series**. **The whole namespace is traced by default.**
Non-float entries log on **exact change**; float entries log on an
**ε-deadband** (default `1e-3`, per-signal override). At end-of-run the table
dumps to per-signal timeseries Python SIL tests evaluate against. Zero-order-hold
semantics. Fast mode keeps the full unbounded trace; realtime mode rolls it to
disk.

---

## 1. Each traced entry is a historian

An entry holds both its current value (live access) and a growing vector of
`(timestamp, value)` samples, appended **only on change** (§3). A signal
constant for 10⁶ ticks stores **one** sample — this is what keeps memory
manageable even when tracing the whole namespace.

The dumped table is the complete timeseries history of every traced signal —
firmware statics, model states, constants — and is the artifact Python test
evaluations run against.

## 2. Semantics: zero-order hold

A sample `(t, v)` means **"the signal became `v` at sim-time `t` and held until
the next sample."** The logical "value at every timestep" is reconstructed by
ZOH; Python recovers the value at any instant or resamples to a uniform grid.
Read gaps as "held," not "missing."

- **Timestamp = tick index** (× base `dt` = sim-time): compact and exact.

## 3. Change detection — exact for discretes, deadband for floats

- **Non-float entries** (int / bool / enum / pointer / FSM state) log on **exact
  change** — any difference appends a sample. The discrete / branch signals D7
  depends on stay exact.
- **Float entries** log on an **ε-deadband**: append only when the value has
  moved more than the deadband from the last logged sample. **Default `1e-3`**
  ("moved, not noise"), **overridable per signal** for finer or coarser
  resolution. Absolute by default; a relative (per-mille) option can come later
  for signals whose magnitude varies widely.

Mechanism by backing kind:

- **Model entries:** framework owns the writes → evaluate the threshold **on
  write**.
- **Firmware entries:** the framework can't see mid-burst writes (no
  instrumentation), so it **samples at tick boundaries** (reads the address,
  applies the threshold vs the last logged value).

**Resolution = base `dt`; sub-tick transients are not captured** — a firmware
static that changes and reverts within one burst is missed. Accepted gap for
now; a future feature could let firmware **inject specific async transients
directly into the State Table** (§9).

## 4. Trace the whole namespace by default

Everything is traced by default — model states + the entire firmware static
namespace. The one **built-in exclusion**: **large raw buffers, chiefly
FreeRTOS task stacks** (`StackType_t[]` arrays that churn every tick — huge and
meaningless to historian). Implemented as a default array-size cap, shipped
from the start.

**Implemented (whole-namespace mirror, no declarations).** The firmware member
enumerates every traceable leaf from DWARF at enable (scalars, recursed struct
members, expanded array elements) and sweeps them all memory→table each tick —
the cvar namespace is an automatic, accurate mirror of firmware memory. The
built-in exclusion is a **default array-size threshold of 32** (arrays larger are
skipped whole, dropping stacks / `ucHeap` / 512-byte buffers), plus multi-dim /
unknown-length arrays and non-data leaves. Per-member `exclude(prefix)` /
`include(path)` tune it. See [`state-route-tables.md`](state-route-tables.md) §1.

- A general **user-facing trace filter** (exclude by symbol / pattern) is a
  later addition; the built-in stack/large-array exclusion is the only one
  needed up front.
- **Cost note:** with everything traced, the per-tick cost is dominated by
  *scanning* every firmware static (read + threshold) — the deadband cuts
  *storage*, not *scan*. Two mitigations (both in [`performance.md`](performance.md)
  Lever 4): **gate the scan on "firmware ran this tick"** (free), and a
  **pluggable change-detector** that swaps naive-scan for **dirty-page tracking**
  (protect `.data`/`.bss`, fault-mark dirty pages, scan only those). Bake the
  seam in now; add dirty-tracking when it profiles hot.

## 5. Deadband ↔ assertion tolerance (couples to D7)

A signal's deadband is the **floor on its assertable resolution**: under ZOH the
trace can be up to `deadband` from the true value, so asserting tighter than the
deadband is meaningless. The defaults are intentionally different scales (trace
`1e-3` vs assertion `1e-6`, [`determinism.md`](determinism.md)), so the rule is:

> **assert-tolerance ≥ the signal's deadband.**

To assert an analog signal tightly, tighten *its* deadband too. The framework
**should warn** when a test asserts tighter than a signal's deadband. Discrete
signals are exact-logged, so this only concerns floats.

## 6. Retention

- **Fast / deterministic mode: unbounded — capture the entire trace.** Held in
  memory and dumped at end-of-run. (Future: dump the whole State Table to an
  **MDF4 `.mf4`** file for plotting.)
- **Realtime mode: bounded memory via a rolling dump-to-file backend.** Chunks
  flush to disk as the run proceeds and are **stitched back into the full trace
  once the sim ends**, so realtime also preserves the complete history — just
  not all in RAM. The dashboard (D4) reads the live series.

## 7. Dump / serialization

- Per-signal: `{ name, type, [(tick, value)...] }`.
- Target export format is **ASAM MDF4 (`.mf4`)** (plays with `asammdf` / CANape /
  INCA and standard measurement tooling). The exact on-disk/in-flight encoding
  is a **just-in-time implementation choice** (Phase 4); the model above is
  what's fixed here. Fast-mode end-dump and realtime rolling-dump should share
  one serialization path.

## 8. Couplings

- **State Table** — the historian is a property of its entries
  ([`state-route-tables.md`](state-route-tables.md)).
- **D7** — deadband bounds assertable resolution (§5); the seed is recorded
  alongside the trace.
- **D10** — scenarios override per-signal deadbands / the trace filter.
- **D4** — the dashboard streams the same series live.

## 9. Open / future

- **Firmware-injected async transients** into the State Table (covers the
  sub-tick gap, §3).
- **User trace filter** (exclude by symbol/pattern) beyond the built-in
  stack/large-array exclusion.
- **Dirty-page tracking** to cut the per-tick scan cost of tracing everything
  (the pluggable change-detector seam — [`performance.md`](performance.md) Lever 4).
- **Relative (per-mille) deadband** option for wide-dynamic-range signals.
- **Assert-tighter-than-deadband warning** in the test layer.
