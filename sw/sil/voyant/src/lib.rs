//! # voyant — clairvoyant software-in-the-loop
//!
//! A generic, firmware-agnostic framework for running cross-compiled embedded
//! firmware in a deterministic virtual world with white-box visibility into its
//! state. A project's instantiation crate supplies the firmware, models, and
//! routes; nothing here is board-specific.
//!
//! Modules:
//! - [`signal`] — [`SignalId`] (`sig_type:source:name[:modifier]`) + the [`Value`] currency.
//! - [`state_table`] — the [`StateTable`]: signal registry + change-logged history +
//!   retention. Pure data, no FFI (it *is* the historian, D12).
//! - `backend` — [`Firmware`] (public handle: control ABI + DWARF cvar resolver, the
//!   only unsafe part) and [`FirmwareMember`] (a firmware wrapped as a [`Member`]). A
//!   "port" is just a Signal the firmware syncs from C — firmware-member vocabulary,
//!   not a voyant primitive. The internal `Backend` trait is the test-double seam.
//! - [`member`] — the [`Member`] trait (the one seam the [`Engine`] drives), plus
//!   [`RampModel`] and [`vsig_id`].
//! - [`route`] — the [`RouteTable`]: `source → destination` transport with per-route
//!   latency (0 = same-tick, 1 = delayed ZOH cut) and suspend/resume for fault injection.
//! - [`engine`] — the [`Engine`]: sim clock + step loop owning table/routes/members,
//!   advancing one tick at a time. Holds no backend handle — each firmware member drives its own.
//! - [`log`] — [`LogLevel`] / [`LogEntry`] + the drop-oldest [`LogRing`].
//!
//! Run modes (fast / realtime pacing) wrap [`Engine::step`] and land in a later chunk.

mod backend;
mod dwarf;
pub mod engine;
pub mod log;
pub mod member;
pub mod route;
pub mod signal;
pub mod state_table;

pub use backend::{Firmware, FirmwareMember};
pub use engine::{Engine, EngineError};
pub use log::{LogEntry, LogLevel, LogRing};
pub use member::{vsig_id, Member, RampModel};
pub use route::{RouteError, RouteTable};
pub use signal::{ParseError, SignalId, Value};
pub use state_table::{StateTable, StateTableConfig, TableError};
