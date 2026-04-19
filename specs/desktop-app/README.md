# Desktop app specs

`app~` requirements: Rust desktop GUI for configuring, operating, and
diagnosing the device. Each spec needs at least one `impl` tag (in Rust
source) and at least one `test` tag (unit and/or integration).

## Topics

Sub-folders are created when a topic gets its first spec.

- `architecture/` — module structure, async runtime, state management
- `connection/` — device discovery, USB protocol decoder, reconnection
  handling
- `views/` — live plot, controls panel, configuration view, diagnostics
  view
- `data/` — stream decoder, session log format, data export

## Tagging conventions in Rust code

```rust
// [impl->app~connection_device_discovery~1]
fn discover_devices() -> Vec<Device> { ... }
```

```rust
// [test->app~connection_device_discovery~1]
#[test]
fn discovery_lists_connected_device() { ... }
```

A single piece of code can carry multiple tags if it implements multiple
specs (rare; usually a sign you should split the module).
