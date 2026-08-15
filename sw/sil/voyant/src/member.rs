//! The [`Member`] trait — the single seam the engine drives every executable
//! participant through — plus the `vsig` helper and [`RampModel`], voyant's
//! reference model member. See `docs/sil/state-route-tables.md`.
//!
//! ## The member model
//!
//! Every executable thing (firmware instance, plant/peer model, future native app)
//! is a [`Member`]; the [`Engine`](crate::engine::Engine) drives it *only* through
//! this trait. There is no `signals()` declaration and no pull-based `read()` — a
//! member treats the [`StateTable`] as a live workspace: it **registers** its signals
//! on the table, **pushes** outputs via [`StateTable::record`], and reads routed
//! inputs via [`StateTable::current_value`]. Registration is open (any `sig_type`,
//! any time); the `sig_type` names only the backing regime, not the member's kind.
//!
//! ## Member discipline (convention, not enforcement)
//!
//! First-party members are trusted to stay in their lane:
//! - Record only signals under **its own `<source>` namespace**
//!   ([`name`](Member::name)); read only its own outputs + routed inputs.
//! - **Never** call [`StateTable::set_time`] or mutate table config (retention/epsilon).
//! - **All cross-member coupling flows through routes**, never by reaching into
//!   another member's signals.
//!
//! Driver / scenario code (the sanity suite) is **exempt** — cross-member injection
//! and sim-time control is its job. This stays convention, not enforcement: a
//! narrowed per-member view would only add friction today. Escalation, *if* scripted
//! members (Python) ever change the trust model, is to hand an enforced narrowed view.
//!
//! ## Registration order is a design surface
//!
//! The engine advances members in **registration order** and resolves forward
//! (zero-latency) dataflow along it, so ordering is deliberate: **put producer
//! before consumer**. You need not get it right by inspection — a zero-latency route
//! reading a value a later member has not produced yet is a backward/feedback edge,
//! and the step-time validator names it and asks you to declare that route delayed
//! (the ZOH cut) or reorder the members.

use crate::duplex::{DuplexHandle, DuplexRouter};
use crate::log::LogLevel;
use crate::signal::{ParseError, SignalId, Value};
use crate::state_table::StateTable;

/// The per-advance context the engine hands each member: the [`StateTable`] plus
/// **duplex access**. A member reads/writes its signals through `ctx.st` and, if it
/// initiates a serial bus, runs a synchronous exchange through
/// [`duplex_transfer`](Self::duplex_transfer) — the model-side twin of the firmware's
/// C SPI upcall. Both paths drive the engine's one shared
/// [`DuplexRouter`](crate::duplex), so a model and a firmware member couple over a bus
/// as peers.
pub struct MemberCtx<'a> {
    /// The State Table — a member's live workspace (register / record / read).
    pub st: &'a mut StateTable,
    /// The shared duplex router (initiate a transfer; the firmware member also
    /// declares + installs its C endpoints here).
    pub(crate) duplex: &'a DuplexRouter,
}

impl<'a> MemberCtx<'a> {
    pub(crate) fn new(st: &'a mut StateTable, duplex: &'a DuplexRouter) -> Self {
        Self { st, duplex }
    }

    /// Run a synchronous duplex transfer on `handle`: `tx` in, the linked peer's `rx`
    /// back this same call. `None` = an unlinked endpoint (a floating bus). Resolve
    /// `handle` once at wiring time (from
    /// [`Engine::link_duplex`](crate::engine::Engine::link_duplex)).
    pub fn duplex_transfer(&mut self, handle: DuplexHandle, tx: &[u8]) -> Option<Vec<u8>> {
        self.duplex.transfer(handle, tx)
    }
}

/// Advance a member with a throwaway (empty) duplex router — for tests whose
/// member registers no duplex endpoints.
#[cfg(test)]
pub(crate) fn advance_unwired(m: &mut dyn Member, dt_us: u64, st: &mut StateTable) {
    let router = DuplexRouter::new();
    m.advance(dt_us, &mut MemberCtx::new(st, &router));
}

/// An executable participant in the sim. The engine drives every member through
/// this trait and nothing else (see the module docs for the member model and the
/// discipline convention).
pub trait Member {
    /// This member's instance name — the `<source>` segment of every signal it
    /// registers (e.g. `motor`, `board_a`). Stable for the member's lifetime.
    /// Two members may share an underlying implementation (two boards running the
    /// same firmware DLL) yet must have distinct names.
    fn name(&self) -> &str;

    /// Advance one deterministic step of `dt_us` microseconds of sim time. The
    /// member reads its routed inputs from `ctx.st` ([`StateTable::current_value`]),
    /// integrates, and pushes its outputs back ([`StateTable::record`]); it may
    /// also register new signals here, and initiate a serial bus via
    /// [`ctx.duplex_transfer`](MemberCtx::duplex_transfer). Must be deterministic:
    /// no wall-clock, no un-seeded RNG.
    fn advance(&mut self, dt_us: u64, ctx: &mut MemberCtx);

    /// Enable or disable the member. The engine calls `set_enabled(true, st)` at add
    /// (members start enabled) and on any re-enable. Registering signals here is the
    /// tidy convention, not a mandate — registration is legal any time, and idempotent.
    /// A disabled member's [`advance`](Member::advance) is skipped while sim time flows
    /// and its signals hold their last value (`set_enabled(false, _)` may do nothing).
    ///
    /// Re-enable **depth** is member-kind-specific: a plain model re-registers as a
    /// benign no-op, while a [`FirmwareMember`](crate::FirmwareMember) with a reload
    /// recipe reboots its image from reset here (statics from scratch, caches rebuilt,
    /// signal history preserved).
    fn set_enabled(&mut self, on: bool, st: &mut StateTable);
}

/// The canonical `vsig:<source>:<local>` id for one of a model member's signals.
pub fn vsig_id(source: &str, local: &str) -> Result<SignalId, ParseError> {
    SignalId::new("vsig", source, local, None)
}

/// A reference model [`Member`]: a linear ramp source for exercising the `vsig`
/// backing in tests and demos. Exposes one signal, `value`, advancing at a fixed
/// slope per second of sim time. Deterministic — real plant models live on the
/// instantiation side.
pub struct RampModel {
    name: String,
    unit: Option<String>,
    slope_per_s: f64,
    elapsed_us: u64,
    value: f64,
}

impl RampModel {
    /// A ramp named `name` whose `value` grows by `slope_per_s` units every second
    /// of sim time, starting at 0.
    pub fn new(name: &str, slope_per_s: f64, unit: Option<&str>) -> Self {
        Self {
            name: name.to_string(),
            unit: unit.map(str::to_string),
            slope_per_s,
            elapsed_us: 0,
            value: 0.0,
        }
    }

    /// The id of this ramp's single `value` signal.
    fn value_id(&self) -> SignalId {
        vsig_id(&self.name, "value").expect("ramp name yields a valid vsig id")
    }
}

impl Member for RampModel {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, dt_us: u64, ctx: &mut MemberCtx) {
        self.elapsed_us += dt_us;
        self.value = self.slope_per_s * (self.elapsed_us as f64) / 1e6;
        // Registered at enable, so this cannot fail normally; on error log a Warning
        // (keeps advance infallible) rather than swallow it.
        let id = self.value_id();
        if let Err(e) = ctx.st.record(&id, Value::F64(self.value)) {
            ctx.st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            // Idempotent: a re-enable re-registers the same signal as a no-op.
            let _ = st.register(self.value_id(), self.unit.as_deref());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vsig_id_is_canonical() {
        let id = vsig_id("motor", "angle_rad").unwrap();
        assert_eq!(id.as_str(), "vsig:motor:angle_rad");
        assert_eq!(id.sig_type(), "vsig");
        assert_eq!(id.source(), "motor");
        assert_eq!(id.name(), "angle_rad");
    }

    #[test]
    fn set_enabled_registers_the_vsig() {
        let mut st = StateTable::new();
        let mut m = RampModel::new("ramp", 1000.0, Some("counts"));
        m.set_enabled(true, &mut st);
        assert_eq!(st.len(), 1);
        let id = vsig_id("ramp", "value").unwrap();
        // Registered but not yet recorded.
        assert_eq!(st.current_value(&id).unwrap(), None);
        // Re-enable is an idempotent no-op (no duplicate error, still one signal).
        m.set_enabled(true, &mut st);
        assert_eq!(st.len(), 1);
    }

    #[test]
    fn advance_records_the_ramp_into_the_historian() {
        let mut st = StateTable::new();
        let mut m = RampModel::new("ramp", 1000.0, None); // +1.0 / ms
        m.set_enabled(true, &mut st);
        let id = vsig_id("ramp", "value").unwrap();

        for tick in 1..=4u64 {
            st.set_time(tick * 1_000);
            advance_unwired(&mut m, 1_000, &mut st);
        }
        // value 1,2,3,4 all beyond epsilon -> four change-log entries.
        assert_eq!(st.changes(&id).unwrap().len(), 4);
        assert_eq!(st.current_value(&id).unwrap(), Some(Value::F64(4.0)));
        // ZOH lookup between records holds the prior sample.
        assert_eq!(st.value_at(&id, 2_500).unwrap(), Some(Value::F64(2.0)));
    }

    #[test]
    fn member_usable_behind_dyn() {
        // The engine drives members behind `dyn Member`; prove object-safety.
        let mut st = StateTable::new();
        let mut m: Box<dyn Member> = Box::new(RampModel::new("ramp", 1.0, None));
        m.set_enabled(true, &mut st);
        st.set_time(1_000_000);
        advance_unwired(m.as_mut(), 1_000_000, &mut st); // 1 s -> value 1.0
        assert_eq!(m.name(), "ramp");
        let id = vsig_id("ramp", "value").unwrap();
        assert_eq!(st.current_value(&id).unwrap(), Some(Value::F64(1.0)));
    }
}
