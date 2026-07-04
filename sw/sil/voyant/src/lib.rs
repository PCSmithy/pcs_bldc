//! # voyant — clairvoyant software-in-the-loop
//!
//! A generic, firmware-agnostic framework for running cross-compiled embedded
//! firmware in a deterministic virtual world, with total white-box visibility
//! into its state. A project (e.g. `pcs_bldc_sil`) instantiates voyant by
//! implementing its trait seams and supplying its firmware, models, and routes;
//! nothing here is specific to any one board.
//!
//! Modules:
//! - [`signal`] — [`SignalId`] (`sig_type:source:name[:modifier]`) + the common
//!   [`Value`] currency.
//! - [`state_table`] — the [`StateTable`]: signal registry + per-signal
//!   change-logged history + current cache + injection overrides + retention
//!   (state-route-tables.md; the State Table *is* the historian, D12). Pure
//!   data — no FFI.
//! - [`backend`] — the [`Backend`] seam (lifecycle + `cvar` read/write) and its
//!   first impl [`Firmware`]: the control ABI + the cvar sample-resolver
//!   (read/write firmware statics as [`Value`], the only unsafe/DWARF part).
//! - [`model`] — the [`Model`] trait + the State Table's **`vsig`** backing:
//!   model-declared signals sampled/recorded like cvars ([`register_model`] /
//!   [`record_model`]), with [`RampModel`] as a reference impl.
//! - [`route`] — the [`RouteTable`]: declarative `source → destination`
//!   transport, propagated once per tick (snapshot-then-write), with per-route
//!   suspend/resume for fault injection (state-route-tables.md §2).
//!
//! The sim clock and run modes build on these.

mod backend;
mod dwarf;
pub mod model;
pub mod route;
pub mod signal;
pub mod state_table;

pub use backend::{Backend, Firmware};
pub use model::{record_model, register_model, vsig_id, Model, ModelError, ModelSignal, RampModel};
pub use route::{RouteError, RouteTable};
pub use signal::{ParseError, SignalId, Value};
pub use state_table::{StateTable, StateTableConfig, TableError};
