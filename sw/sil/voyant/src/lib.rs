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
//! - [`backend`] — the native firmware backend ([`Firmware`]): the control ABI
//!   + the cvar sample-resolver (read/write firmware statics as [`Value`], the
//!   only unsafe/DWARF part).
//!
//! The Route Table, sim clock, and run modes build on these.

mod backend;
mod dwarf;
pub mod signal;
pub mod state_table;

pub use backend::Firmware;
pub use signal::{ParseError, SignalId, Value};
pub use state_table::{StateTable, StateTableConfig, TableError};
