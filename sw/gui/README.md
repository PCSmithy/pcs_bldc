# pcs_bldc — GUI operator app

A Rust + webview (Tauri 2) desktop app for connecting to the pcs_bldc
board: observability (DWARF signal selection, live trace plotting,
telemetry, logs), command, and diagnosis over the USB serial protocol.
Requirements live in `specs/desktop-app/` (`app~` specs).

Layout:

- `src-tauri/` — the native core (own Cargo workspace + lockfile). All
  device I/O, protocol state, and loaded-ELF state lives here
  (`app~arch_001~1`); shared logic comes from the `sw/lib/rust` crates
  (`pcs_wire`, `pcs_proto`, `dwarf_map`).
- `dist/` — the static webview frontend (no JS toolchain; plain
  HTML/JS served via `frontendDist`, `withGlobalTauri` for `invoke`).

Build/run from `src-tauri/`: `cargo build`, `cargo tauri dev` (or
`cargo run` for the plain window).
