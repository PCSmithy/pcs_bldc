//! # voyant — clairvoyant software-in-the-loop
//!
//! A generic, firmware-agnostic framework for running cross-compiled embedded
//! firmware in a deterministic virtual world, with total white-box visibility
//! into its state. A project's own instantiation crate instantiates voyant by
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
//! - [`backend`] — the [`Backend`] seam (lifecycle + `cvar` read/write + the
//!   **port** registration seam: signals a firmware's sim HW drivers register
//!   at runtime through a hook vtable, carried in native units and
//!   cache-mediated around each tick) and its first impl [`Firmware`]: the
//!   control ABI + the cvar sample-resolver (read/write firmware statics as
//!   [`Value`], the only unsafe/DWARF part). Also [`FirmwareMember`]: a
//!   firmware instance wrapped as a [`Member`], syncing cvar mirrors and port
//!   caches around its firmware tick.
//! - [`member`] — the [`Member`] trait, the single seam the [`Engine`] drives
//!   every participant through (register signals on the table, push outputs, read
//!   routed inputs), plus [`RampModel`], voyant's reference model member, and the
//!   [`vsig_id`] helper.
//! - [`route`] — the [`RouteTable`]: declarative `source → destination`
//!   transport with per-route latency (0 = same-tick forward dataflow, 1 = the
//!   delayed ZOH cut), step-time validation, and per-route suspend/resume for
//!   fault injection (state-route-tables.md §2–§3).
//! - [`engine`] — the [`Engine`]: the sim clock + step loop that owns the State
//!   Table / Route Table / members and advances the whole system one tick at a
//!   time in the canonical order — delayed routes from a pre-tick snapshot, then
//!   per member the zero-latency DAG resolved fresh in topo order + advance
//!   (state-route-tables.md §3). It holds no backend handle — each firmware member
//!   drives its own backend.
//! - [`log`] — the unified log system: [`LogLevel`] / [`LogEntry`] + the
//!   drop-oldest [`LogRing`] the [`StateTable`] stamps with sim time.
//!
//! Run modes (fast / realtime pacing) are a thin wrapper over [`Engine::step`]
//! and land in a later chunk.

mod backend;
mod dwarf;
pub mod engine;
pub mod log;
pub mod member;
pub mod route;
pub mod signal;
pub mod state_table;

pub use backend::{Backend, Firmware, FirmwareMember, PortDef};
pub use engine::{Engine, EngineError};
pub use log::{LogEntry, LogLevel, LogRing};
pub use member::{vsig_id, Member, RampModel};
pub use route::{RouteError, RouteTable};
pub use signal::{ParseError, SignalId, Value};
pub use state_table::{StateTable, StateTableConfig, TableError};
