//! The sim clock + step loop: the engine that ties the parts together.
//!
//! [`Engine`] owns the [`StateTable`], the [`RouteTable`], and a list of
//! [`Member`]s (every executable participant — firmware instances, plant/peer
//! models, future native apps), and advances the whole system one tick at a time
//! with [`step`](Engine::step). It is the "sim core" the design docs describe
//! (`architecture.md` §5, `state-route-tables.md` §3), and it drives members
//! *only* through the [`Member`] trait — an **engine of members**, agnostic to
//! what any member actually is.
//!
//! ## The canonical tick order
//!
//! Each [`step`](Engine::step) advances sim time by one fixed tick period, then
//! walks the members in **registration order** (deterministic, D7). For each
//! **enabled** member it **propagates routes, then advances the member**:
//!
//! 1. **advance sim time** — `now += tick_period`, monotonic and wall-clock-free
//!    (D7/D9); [`StateTable::set_time`] stamps every record this tick.
//! 2. **for each enabled member, in order:** first **propagate routes**
//!    (snapshot-then-write, purely on the State Table) so the member sees the
//!    freshest routed inputs at its turn, then **`member.advance(dt, st)`** — the
//!    member reads its routed inputs, steps, and pushes its outputs (a firmware
//!    member also flushes its *driven* cvars into firmware memory, runs
//!    `advance_tick`, and samples its cvars back out here).
//!
//! Disabled members are skipped while sim time keeps flowing (their signals hold
//! their last value); see [`set_member_enabled`](Engine::set_member_enabled).
//!
//! ### Route placement is an INTERIM placeholder — NOT the final design
//!
//! Propagating routes *before each member* preserves today's semantics with zero
//! added latency along registration order: a model member's output routed to a
//! firmware member's `cvar` lands **before** that firmware's `advance_tick` in the
//! *same* step, because the firmware member is advanced after the model that feeds
//! it. That is the load-bearing ordering guarantee the tests pin.
//!
//! **This placement is explicitly interim.** The owner has an OPEN design question
//! on route-hop latency: the "one tick per hop" delay stated in
//! `state-route-tables.md` §3 is **not settled**, and the standing requirement is
//! that routing add **no artificially induced per-hop latency**. Where a route's
//! source is produced later in registration order than its consumer, freshness
//! follows that order — a future ordering/latency design (a later chunk, after an
//! owner design session) may replace this. Do not treat before-each-member as the
//! final rule.
//!
//! ## No backend handle (routes are table-mediated)
//!
//! The engine touches **only** members, routes, and the table — it holds **no
//! [`Backend`](crate::Backend) borrow**. Route propagation is a pure State Table
//! operation ([`RouteTable::propagate`]): it records values between table entries
//! and nothing more. Each firmware instance lives inside its own
//! [`FirmwareMember`](crate::FirmwareMember) and drives *its own* backend — it
//! flushes routed `cvar` destinations into firmware memory (and samples firmware
//! outputs back out) inside its own `advance`. This removes the last
//! single-firmware assumption: multi-firmware is simply multiple `FirmwareMember`s,
//! each flushing/sampling through its own backend, all peers in one engine.
//!
//! [`RouteTable::propagate`]: crate::route::RouteTable::propagate

use crate::log::LogEntry;
use crate::member::Member;
use crate::route::{RouteError, RouteTable};
use crate::signal::SignalId;
use crate::state_table::StateTable;
use thiserror::Error;

/// A registered member plus the engine's own enable flag (engine-gating: the
/// engine skips a disabled member's [`Member::advance`] — simpler than making
/// every member self-gate).
struct MemberEntry<'m> {
    member: Box<dyn Member + 'm>,
    enabled: bool,
}

/// Errors from engine setup and stepping.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum EngineError {
    #[error(transparent)]
    Route(#[from] RouteError),
}

/// The sim clock + step loop. Owns the State Table (the historian), the Route
/// Table, and the members — and **nothing else**: it holds no backend handle (see
/// the module "No backend handle" note). Each firmware member drives its own
/// backend, so a caller can keep its own `&Backend` for ad-hoc white-box
/// injection / inspection alongside the engine without contending with it.
///
/// The `'b` lifetime bounds the members (a [`FirmwareMember`](crate::FirmwareMember)
/// borrows its backend for `'b`); it no longer appears on the engine's own fields.
pub struct Engine<'b> {
    state: StateTable,
    routes: RouteTable,
    members: Vec<MemberEntry<'b>>,
    tick_period_us: u64,
    now_us: u64,
}

impl<'b> Engine<'b> {
    /// A new engine advancing `tick_period_us` of sim time per [`step`](Self::step)
    /// (e.g. 1000 for a 1 kHz cadence). Sim time starts at 0; the first `step`
    /// records at `tick_period_us`.
    pub fn new(tick_period_us: u64) -> Self {
        Self {
            state: StateTable::new(),
            routes: RouteTable::new(),
            members: Vec::new(),
            tick_period_us,
            now_us: 0,
        }
    }

    /// A new engine with a caller-configured [`StateTable`] (retention / epsilon /
    /// log capacity).
    pub fn with_state(tick_period_us: u64, state: StateTable) -> Self {
        Self {
            state,
            ..Self::new(tick_period_us)
        }
    }

    /// Add a member. Members **start enabled**: the engine calls
    /// [`Member::set_enabled(true)`](Member::set_enabled) now, which is where the
    /// member registers its signals on the State Table. Advance order is
    /// registration order (deterministic, D7).
    pub fn add_member(&mut self, mut member: Box<dyn Member + 'b>) {
        member.set_enabled(true, &mut self.state);
        self.members.push(MemberEntry {
            member,
            enabled: true,
        });
    }

    /// Enable or disable a member by name. A disabled member's advance is skipped
    /// each step (its signals hold their last value); re-enabling calls
    /// [`Member::set_enabled(true)`](Member::set_enabled) again (which idempotently
    /// re-registers its signals). Returns whether a member of that name was found.
    pub fn set_member_enabled(&mut self, name: &str, on: bool) -> bool {
        if let Some(entry) = self.members.iter_mut().find(|e| e.member.name() == name) {
            entry.enabled = on;
            entry.member.set_enabled(on, &mut self.state);
            true
        } else {
            false
        }
    }

    /// Add a `src → dst` route (delegates to the [`RouteTable`]; `dst` may be any
    /// registered signal, validated at propagate — see [`RouteTable::add`]).
    pub fn add_route(&mut self, src: SignalId, dst: SignalId) -> Result<(), EngineError> {
        self.routes.add(src, dst)?;
        Ok(())
    }

    /// Suspend a route (stop driving its destination) for fault injection.
    pub fn suspend_route(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), EngineError> {
        self.routes.suspend(src, dst)?;
        Ok(())
    }

    /// Resume a suspended route.
    pub fn resume_route(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), EngineError> {
        self.routes.resume(src, dst)?;
        Ok(())
    }

    /// Remove a route by its endpoints (delegates to [`RouteTable::remove`]).
    pub fn remove_route(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), EngineError> {
        self.routes.remove(src, dst)?;
        Ok(())
    }

    /// Advance the whole system one tick (the canonical order — see the module
    /// docs). Each call moves sim time forward by one period.
    pub fn step(&mut self) -> Result<(), EngineError> {
        // 1. Sim time advances (monotonic, deterministic — no wall-clock, D7/D9).
        self.now_us += self.tick_period_us;
        self.state.set_time(self.now_us);

        // 2. Each enabled member, in registration order: propagate routes so it
        //    sees the freshest routed inputs (INTERIM placement — see module docs),
        //    then advance it. `routes` and `state` are disjoint fields from
        //    `members`, so these borrows coexist with the iterator. Propagation is
        //    table-only (no backend); the member syncs its own firmware mirrors.
        let dt = self.tick_period_us;
        for entry in &mut self.members {
            if !entry.enabled {
                continue;
            }
            self.routes.propagate(&mut self.state)?;
            entry.member.advance(dt, &mut self.state);
        }
        Ok(())
    }

    /// Current sim time (microseconds), monotonic across [`step`](Self::step)s.
    pub fn now_us(&self) -> u64 {
        self.now_us
    }

    /// The tick period (microseconds).
    pub fn tick_period_us(&self) -> u64 {
        self.tick_period_us
    }

    /// The State Table / historian, for assertions and inspection.
    pub fn state(&self) -> &StateTable {
        &self.state
    }

    /// Drain the State Table's buffered log entries (sim-time-stamped warnings /
    /// info from members and the engine). See [`StateTable::take_logs`].
    ///
    /// [`StateTable::take_logs`]: crate::state_table::StateTable::take_logs
    pub fn take_logs(&mut self) -> Vec<LogEntry> {
        self.state.take_logs()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::{Backend, FirmwareMember};
    use crate::member::{vsig_id, RampModel};
    use crate::signal::Value;
    use crate::state_table::TableError;
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;
    use std::rc::Rc;

    /// A pure-Rust [`Backend`] that records the order of its calls, so a test can
    /// prove *when* in a step a route write vs `advance_tick` happens. Reads return
    /// a stored value if written, else a `U32` of the tick count (a firmware-less
    /// ramp for the sampling tests).
    #[derive(Default)]
    struct MockBackend {
        log: RefCell<Vec<String>>,
        ticks: Cell<u32>,
        cvars: RefCell<HashMap<String, Value>>,
    }

    impl Backend for MockBackend {
        fn start(&self) -> bool {
            true
        }
        fn advance_tick(&self) {
            self.ticks.set(self.ticks.get() + 1);
            self.log.borrow_mut().push("advance_tick".into());
        }
        fn shutdown(&self) {}
        fn read_cvar(&self, path: &str) -> Value {
            self.log.borrow_mut().push(format!("read:{path}"));
            self.cvars
                .borrow()
                .get(path)
                .cloned()
                .unwrap_or(Value::U32(self.ticks.get()))
        }
        fn write_cvar(&self, path: &str, v: &Value) {
            self.log.borrow_mut().push(format!("write:{path}"));
            self.cvars.borrow_mut().insert(path.to_string(), v.clone());
        }
    }

    /// A model member that appends its name to a shared log when advanced — for
    /// proving multi-member advance order. Registers no signals.
    struct OrderModel {
        name: String,
        log: Rc<RefCell<Vec<String>>>,
    }

    impl Member for OrderModel {
        fn name(&self) -> &str {
            &self.name
        }
        fn advance(&mut self, _dt_us: u64, _st: &mut StateTable) {
            self.log.borrow_mut().push(self.name.clone());
        }
        fn set_enabled(&mut self, _on: bool, _st: &mut StateTable) {}
    }

    fn cvar(name: &str) -> SignalId {
        SignalId::new("cvar", "test", name, None).unwrap()
    }

    #[test]
    fn step_flushes_driven_dest_before_advance_tick() {
        // The load-bearing ordering guarantee: a model member's output routed to a
        // firmware member's DRIVEN cvar must reach firmware memory BEFORE its
        // advance_tick in the same step. Member order [model, firmware] makes it
        // hold — the model records its vsig, propagate (before the firmware member)
        // records it into the cvar entry, and the firmware member flushes it.
        let be = MockBackend::default();
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(RampModel::new("ramp", 1000.0, None)));
        let mut fw = FirmwareMember::new("fw", &be, 1_000);
        fw.drive_cvar(cvar("sensor_in"), None);
        eng.add_member(Box::new(fw));
        eng.add_route(vsig_id("ramp", "value").unwrap(), cvar("sensor_in"))
            .unwrap();

        eng.step().unwrap();

        let log = be.log.borrow();
        let w = log.iter().position(|e| e == "write:sensor_in").unwrap();
        let t = log.iter().position(|e| e == "advance_tick").unwrap();
        assert!(w < t, "driven flush must precede advance_tick, got {log:?}");
        // And firmware memory actually received the model's first-tick value.
        assert_eq!(be.cvars.borrow().get("sensor_in"), Some(&Value::F64(1.0)));
    }

    #[test]
    fn time_advances_by_tick_period() {
        let be = MockBackend::default();
        let mut eng = Engine::new(1_000);
        let id = cvar("counter");
        let mut fw = FirmwareMember::new("fw", &be, 1_000);
        fw.sample_cvar(id.clone(), None);
        eng.add_member(Box::new(fw));

        assert_eq!(eng.now_us(), 0);
        for tick in 1..=4u64 {
            eng.step().unwrap();
            assert_eq!(eng.now_us(), tick * 1_000);
        }
        // The mock's read ramps with the tick count, so the historian holds a
        // sample per tick, timestamped at each period boundary.
        let changes = eng.state().changes(&id).unwrap();
        assert_eq!(changes.len(), 4);
        assert_eq!(changes[0], (1_000, Value::U32(1)));
        assert_eq!(changes[3], (4_000, Value::U32(4)));
    }

    #[test]
    fn multi_member_advances_in_registration_order() {
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(1_000);
        for name in ["first", "second", "third"] {
            eng.add_member(Box::new(OrderModel {
                name: name.to_string(),
                log: Rc::clone(&log),
            }));
        }

        eng.step().unwrap();
        eng.step().unwrap();

        // Registration order, deterministically, every tick.
        assert_eq!(
            *log.borrow(),
            vec!["first", "second", "third", "first", "second", "third"]
        );
    }

    #[test]
    fn samples_registered_cvars_into_historian() {
        let be = MockBackend::default();
        let mut eng = Engine::new(1_000);
        let id = cvar("ramp_counter");
        let mut fw = FirmwareMember::new("fw", &be, 1_000);
        fw.sample_cvar(id.clone(), Some("counts"));
        eng.add_member(Box::new(fw));

        for _ in 0..3 {
            eng.step().unwrap();
        }
        // Read after advance_tick ⇒ the post-tick counts 1,2,3.
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(&Value::U32(3)));
        assert_eq!(eng.state().changes(&id).unwrap().len(), 3);
    }

    #[test]
    fn records_model_vsig_each_tick() {
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(RampModel::new("ramp", 1000.0, Some("counts"))));
        let id = vsig_id("ramp", "value").unwrap();

        // Registered but unrecorded before the first step.
        assert_eq!(eng.state().current_value(&id).unwrap(), None);
        for _ in 0..5 {
            eng.step().unwrap();
        }
        assert_eq!(eng.state().changes(&id).unwrap().len(), 5);
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(&Value::F64(5.0)));
        // ZOH between records holds the prior sample.
        assert_eq!(eng.state().value_at(&id, 2_500).unwrap(), Some(&Value::F64(2.0)));
    }

    #[test]
    fn empty_step_advances_firmware_and_time() {
        // A lone firmware member with no sampled cvars: a step is just time +
        // advance_tick (no routes, so no writes).
        let be = MockBackend::default();
        let mut eng = Engine::new(2_000);
        eng.add_member(Box::new(FirmwareMember::new("fw", &be, 2_000)));
        eng.step().unwrap();
        eng.step().unwrap();
        assert_eq!(eng.now_us(), 4_000);
        assert_eq!(be.ticks.get(), 2);
        assert_eq!(*be.log.borrow(), vec!["advance_tick", "advance_tick"]);
    }

    #[test]
    fn disabled_member_is_skipped_then_resumes() {
        let be = MockBackend::default();
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(FirmwareMember::new("fw", &be, 1_000)));

        assert!(eng.set_member_enabled("fw", false));
        eng.step().unwrap(); // skipped: no advance_tick
        assert_eq!(be.ticks.get(), 0);
        assert_eq!(eng.now_us(), 1_000); // but sim time still flows

        assert!(eng.set_member_enabled("fw", true));
        eng.step().unwrap();
        assert_eq!(be.ticks.get(), 1);

        // Unknown member name is reported, not panicked.
        assert!(!eng.set_member_enabled("nope", false));
    }

    #[test]
    fn route_source_unregistered_errors() {
        let be = MockBackend::default();
        let mut eng = Engine::new(1_000);
        // A firmware member makes the step propagate routes; the route source is a
        // cvar that is never registered/sampled — a wiring bug.
        eng.add_member(Box::new(FirmwareMember::new("fw", &be, 1_000)));
        eng.add_route(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::Table(TableError::UnknownSignal(_))))
        ));
    }
}
