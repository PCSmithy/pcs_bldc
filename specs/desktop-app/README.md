# Desktop app specs

`app~` requirements: the Rust + webview desktop app (`sw/gui/`) for
observing, commanding, and configuring the device. Each spec needs at
least one `impl` tag (in Rust source) and at least one `test` tag (unit
and/or integration).

## Topics

Sub-folders are created when a topic gets its first spec.

- `arch/` — process split and state ownership ([[core-ownership]]:
  device and firmware state live in the native core;
  [[session-restore]]: the saved port/ELF/watch-list context)
- `conn/` — device session and protocol client ([[session]]), framing
  + envelope codec ([[wire]])
- `obs/` — the acquisition model: ELF/DWARF signal selection
  ([[signal-picker]]), the build-identity gate ([[access-gate]]), the
  trace client and Samples demultiplexer ([[trace-client]])
- `views/` — presentation surfaces: [[live-plot]] (plot, axes, trace
  appearance, decimation), [[telemetry]], [[log]], [[workspace]],
  [[cursor]] (cursor, pointed trace, comparison anchor + deltas),
  [[table]], [[timeline]], [[watch-panel]], [[render-budget]]
  (rendering performance floor), and [[value-rendering]] (the
  surfaces' shared value formatting)

Convention: `obs/` holds acquisition and access — what bytes mean and
when capabilities are available; `views/` holds presentation surfaces —
what the user sees.

## Tagging conventions in Rust code

```rust
// [impl->app~conn_001~1]
fn open_session(port: &str) -> Result<Session> { ... }
```

```rust
// [test->app~conn_001~1]
#[test]
fn session_reopens_after_port_returns() { ... }
```

A single piece of code can carry multiple tags if it implements multiple
specs (rare; usually a sign you should split the module).
