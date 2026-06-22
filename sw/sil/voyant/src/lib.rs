//! # voyant — clairvoyant software-in-the-loop
//!
//! A generic, firmware-agnostic framework for running cross-compiled embedded
//! firmware in a deterministic virtual world, with total white-box visibility
//! into its state. A project (e.g. `pcs_bldc_sil`) instantiates voyant by
//! implementing its trait seams and supplying its firmware, models, and routes;
//! nothing here is specific to any one board.
//!
//! Modules:
//! - [`value`] — the common `Value` currency + scalar memory access.
//! - [`backend`] — the native firmware backend ([`Firmware`]): control ABI +
//!   DWARF white-box read/write (ffi-boundary.md).
//! - the DWARF reader (internal) — resolve `var.member`/`arr[i]` paths.
//! - [`state`] — the State Table ([`StateTable`], [`Signal`]) over the common
//!   `Value` (state-route-tables.md).
//!
//! The Route Table, sim clock, historian, and run modes build on these.

mod backend;
mod dwarf;
mod state;
mod value;

pub use backend::Firmware;
pub use state::{Signal, SignalKey, StateTable};
pub use value::{Scalar, Value};
