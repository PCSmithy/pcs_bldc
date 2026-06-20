# D12 — Signal trace / State Table historian (resolved design)

Resolves open decision **D12** from [`architecture.md`](architecture.md): what
gets recorded, at what rate, and in what form. The answer is that the **State
Table is the historian** — there is no separate trace subsystem.

**Decision:** every *traced* State Table entry
([`state-route-tables.md`](state-route-tables.md)) accumulates a **change-logged,
timestamped time series** — a new `(timestamp, value)` sample is appended only
when the value changes. At end-of-run the table dumps to per-signal timeseries
that Python SIL tests evaluate against. Zero-order-hold semantics; lossless by
default; bounded runs keep full history, long realtime sessions use a retention
window.

---

## 1. Each traced entry is a historian

An entry holds both its current value (live access) and a growing vector of
`(timestamp, value)` samples, appended **only on change**. A signal constant
for 10⁶ ticks stores **one** sample, not 10⁶ — this is what keeps the structure's
memory manageable across the whole namespace.

The dumped table is the complete timeseries history of every traced signal —
firmware statics, model states, constants — and is the artifact Python test
evaluations run against.

## 2. Semantics: zero-order hold

A sample `(t, v)` means **"the signal became `v` at sim-time `t` and held until
the next sample."** The logical "value at every timestep" is reconstructed from
the compressed log by ZOH; Python recovers the value at any instant, or
resamples to a uniform grid for comparison. Test authors must read gaps as
"held," not "missing."

- **Timestamp = tick index** (× base `dt` = sim-time): compact and exact.

## 3. Change detection — by backing kind

- **Model entries:** the framework owns the writes → **append on write**.
- **Firmware entries:** the framework can't see mid-burst writes (no
  instrumentation), so it **samples at tick boundaries** (reads the address,
  diffs against the last logged value) and appends on change.

**Resolution = base `dt`.** Sub-tick transients (a firmware static that changes
and reverts within one burst) are not captured — consistent with the
quantized-time model ([`sim-interrupts.md`](sim-interrupts.md) §5), but a real
limitation to keep in mind.

## 4. Traced subset, not the whole namespace

The full State Table namespace is cheap metadata; only a **selected trace set**
accumulates history. Default = all model states + all routed/watched firmware
signals; the scenario can add/remove signals (D10). The thousands of
FreeRTOS/HAL internals are *not* historianed unless explicitly asked for.

## 5. Lossless by default; optional deadband

- **"Changed" = exact inequality** by default → lossless trace.
- **Caveat — continuous floats log at full rate.** A current or rotor angle
  changes every tick, so change-logging saves nothing there (one sample per
  tick). Unavoidable if you want them faithfully; the compression win is the
  many discrete/slow/constant signals.
- **Optional per-signal ε-deadband** ("log only if |Δ| > ε") compresses analog
  signals at a fidelity cost. **Off by default**; opt in per signal where the
  loss is acceptable.

## 6. Retention

- **Bounded fast-mode runs** keep full history in memory and dump at end — fine.
- **Long realtime sessions** would grow unbounded (analog at full rate), so they
  use a **retention window / stream-to-disk** to bound memory. The dashboard
  (D4) reads the same series live.

## 7. Dump / serialization

- The dump is per-signal: `{ name, [(tick, value)...] }` with the entry's type.
- The on-disk encoding (columnar Arrow/Parquet vs a simple binary vs per-signal
  arrays) is a **just-in-time implementation choice**, picked when the Python
  reader is built (Phase 4). The *model* above is what's fixed here.

## 8. Couplings

- **State Table** — the historian is a property of its entries
  ([`state-route-tables.md`](state-route-tables.md)).
- **D10** — scenarios pick the trace set + per-signal deadband/tolerances.
- **D7** — the recorded trace is what tolerance assertions run against; the seed
  is recorded alongside.
- **D4** — the dashboard streams the same series live.
