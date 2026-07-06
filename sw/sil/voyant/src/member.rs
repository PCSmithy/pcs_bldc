//! The [`Member`] trait — the single seam through which the engine drives every
//! executable participant in the sim — plus the `vsig` helper and [`RampModel`],
//! voyant's reference model member.
//!
//! ## The member model
//!
//! Every executable thing in the sim (a firmware instance, a plant/peer model, a
//! future native app) is a [`Member`]. The [`Engine`](crate::engine::Engine)
//! interacts with members *only* through this trait: it advances them in
//! registration order and toggles their enable state.
//!
//! There is deliberately **no `signals()` declaration and no pull-based `read()`**.
//! A member instead treats the [`StateTable`] as a live workspace:
//!
//! - It **registers** its signals as a runtime act directly on the table (inside
//!   [`set_enabled`](Member::set_enabled) / [`advance`](Member::advance)).
//! - It **pushes** its outputs via [`StateTable::record`] during
//!   [`advance`](Member::advance).
//! - It reads routed inputs via [`StateTable::current_value`].
//!
//! Registration is *open*: any member may register a signal of any `sig_type`
//! (`cvar`, `vsig`, a future transport type) at any time, and nothing infers a
//! member's kind from the `sig_type` it registers. The `sig_type` names only the
//! signal's *backing regime*; the State Table is the flat, member-agnostic signal
//! registry (`docs/sil/state-route-tables.md`).
//!
//! ## Member discipline (convention, not enforcement)
//!
//! First-party members are trusted to stay in their lane:
//!
//! - A member records only signals under **its own `<source>` namespace**
//!   ([`name`](Member::name)), and reads only *its own* signals — its outputs and
//!   the routed inputs targeting it.
//! - A member **never calls [`StateTable::set_time`]** and never mutates table
//!   configuration (retention / epsilon).
//! - **All cross-member coupling flows through routes**, never through one member
//!   reaching into another's signals.
//!
//! Driver / scenario code (e.g. the sanity suite) is **exempt** — deliberate
//! cross-member injection and sim-time control is exactly its job, and it
//! legitimately needs the full table.
//!
//! This is convention by design, not enforcement: members are first-party trusted
//! code and the driver needs the whole table, so a narrowed, per-member table view
//! would only add friction today. The planned escalation, *if* scripted members
//! (Python bindings) ever change the trust model, is to hand a member an enforced
//! narrowed view instead of the full table.
//!
//! ## Registration order is a design surface
//!
//! The [`Engine`](crate::engine::Engine) advances members in **registration order**,
//! and forward (zero-latency) route dataflow is resolved along that order. So the
//! order you add members is a deliberate design choice: **order members along the
//! signal flow** (producer before consumer). You do not have to get it right by
//! inspection — the engine's step-time validator tells you when you get it wrong: a
//! zero-latency route that reads a value a later-ordered member has not produced yet
//! is a backward/feedback edge, and the validator names it and asks you to either
//! declare that route delayed (the ZOH cut) or reorder the members. See
//! [`state-route-tables.md`](../state-route-tables.md) §3.

use crate::log::LogLevel;
use crate::signal::{ParseError, SignalId, Value};
use crate::state_table::StateTable;

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
    /// member reads its routed inputs from `st` ([`StateTable::current_value`]),
    /// integrates, and pushes its outputs back ([`StateTable::record`]); it may
    /// also register new signals here. Must be deterministic (D7): no wall-clock,
    /// no un-seeded RNG.
    fn advance(&mut self, dt_us: u64, st: &mut StateTable);

    /// Enable or disable the member. The engine calls `set_enabled(true, st)` when
    /// the member is added (members start enabled) and again on any re-enable.
    ///
    /// Registering signals here is the **typical convention**, not a mandate:
    /// registration is legal **at any time during runtime** — any member, any
    /// `sig_type`, mid-[`advance`](Member::advance) included (a member may add a
    /// port it just discovered it needs, or a firmware member re-derive its cvars
    /// across a reboot). `set_enabled(true)` is simply the common, tidy place to do
    /// it. Registration is idempotent, so a re-enable is a benign no-op on
    /// already-registered signals.
    ///
    /// Enable only *gates advance* today: the engine skips a disabled member's
    /// [`advance`](Member::advance) while sim time keeps flowing, and the member's
    /// signals hold their last recorded value.
    ///
    /// FUTURE: member-kind-specific re-enable *depth* — a firmware member could
    /// reload its DLL (boot-from-reset), a model reinit its integration state — is
    /// not implemented. For now `set_enabled(false, _)` need do nothing beyond
    /// letting the engine gate the member out.
    fn set_enabled(&mut self, on: bool, st: &mut StateTable);
}

/// The canonical `vsig:<source>:<local>` id for one of a model member's signals.
pub fn vsig_id(source: &str, local: &str) -> Result<SignalId, ParseError> {
    SignalId::new("vsig", source, local, None)
}

/// A reference model [`Member`]: a linear ramp source, for exercising the `vsig`
/// backing in tests and demos. Exposes one signal, `value`, that advances at a
/// fixed slope per second of sim time. Deterministic, no dependencies — real plant
/// models live on the instantiation side.
///
/// It registers its `vsig` in [`set_enabled(true)`](Member::set_enabled) (the
/// engine calls that when the member is added) and pushes a record each
/// [`advance`](Member::advance).
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

    fn advance(&mut self, dt_us: u64, st: &mut StateTable) {
        self.elapsed_us += dt_us;
        self.value = self.slope_per_s * (self.elapsed_us as f64) / 1e6;
        // Registered in set_enabled(true) at add-time, so this cannot fail in
        // normal operation; on error log a Warning (keeps advance infallible and
        // deterministic) rather than swallow it.
        let id = self.value_id();
        if let Err(e) = st.record(&id, Value::F64(self.value)) {
            st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
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
            m.advance(1_000, &mut st);
        }
        // value 1,2,3,4 all beyond epsilon -> four change-log entries.
        assert_eq!(st.changes(&id).unwrap().len(), 4);
        assert_eq!(st.current_value(&id).unwrap(), Some(&Value::F64(4.0)));
        // ZOH lookup between records holds the prior sample.
        assert_eq!(st.value_at(&id, 2_500).unwrap(), Some(&Value::F64(2.0)));
    }

    #[test]
    fn member_usable_behind_dyn() {
        // The engine drives members as `Box<dyn Member>`; prove object-safety.
        let mut st = StateTable::new();
        let mut m: Box<dyn Member> = Box::new(RampModel::new("ramp", 1.0, None));
        m.set_enabled(true, &mut st);
        st.set_time(1_000_000);
        m.advance(1_000_000, &mut st); // 1 s -> value 1.0
        assert_eq!(m.name(), "ramp");
        let id = vsig_id("ramp", "value").unwrap();
        assert_eq!(st.current_value(&id).unwrap(), Some(&Value::F64(1.0)));
    }
}
