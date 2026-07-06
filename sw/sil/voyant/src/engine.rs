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
//! ## The canonical tick order (settled)
//!
//! Discrete sim serializes concurrent physics, so every feedback cycle needs
//! exactly one tick of separation somewhere. The design makes that cut explicit and
//! physical — a **delayed (latency-1) route** models the real ZOH sample/actuation
//! boundary — while forward dataflow (zero-latency routes) has **zero** added
//! latency. Each [`step`](Engine::step):
//!
//! 1. **Advance sim time** — `now += tick_period`, monotonic and wall-clock-free
//!    (D7/D9); [`StateTable::set_time`] stamps every record this tick.
//! 2. **Validate the wiring if dirty** (see below). A cached invalid verdict is
//!    re-raised from `step` until the wiring is fixed.
//! 3. **Evaluate delayed routes once**, from a snapshot taken *before any member
//!    advances* ([`RouteTable::propagate_delayed`]): each delayed destination
//!    receives its source value **as of the end of the previous tick**.
//! 4. **For each enabled member, in registration order:** re-evaluate the enabled
//!    **zero-latency** routes in topological order with fresh reads
//!    ([`RouteTable::propagate_zero_latency`]) — a chain `a→b→c` resolves fully this
//!    same tick — then **`member.advance(dt, st)`**. The member reads its routed
//!    inputs, steps, and pushes its outputs (a firmware member also flushes its
//!    *driven* cvars into firmware memory, runs `advance_tick`, and samples its
//!    cvars back out here).
//!
//! Re-running the full zero-latency DAG before each member is semantically identical
//! to per-member incoming-route resolution (routes are pure copies and `record`
//! dedups unchanged values) and far simpler. The `M×R` copy cost is a flagged perf
//! seam — fine at current scale; `docs/sil/performance.md` owns later optimization.
//!
//! Disabled members are skipped while sim time keeps flowing (their signals hold
//! their last value); see [`set_member_enabled`](Engine::set_member_enabled).
//!
//! ## Step-time validation (dirty-flag cached)
//!
//! Wiring mutations (route add/remove/suspend/resume, member add, member
//! enable/disable) set a **dirty flag**; the next `step` revalidates
//! ([`RouteTable::validate`]) only when dirty, else reuses the cached verdict and
//! the cached zero-latency topological order. Rewiring at any time stays legal —
//! a validation failure surfaces at the *next* step, loudly, as an
//! [`EngineError::Route`] naming the offending route (single-driver, zero-latency
//! acyclicity, forward-flow; see [`RouteTable::validate`]). Member **registration
//! order is a design surface**: order members along the signal flow, and the
//! validator tells you when a route needs to be delayed or the members reordered.
//!
//! ## No backend handle (routes are table-mediated)
//!
//! The engine touches **only** members, routes, and the table — it holds **no
//! [`Backend`](crate::Backend) borrow**. Route propagation is a pure State Table
//! operation: it records values between table entries and nothing more. Each
//! firmware instance lives inside its own
//! [`FirmwareMember`](crate::FirmwareMember) and drives *its own* backend, so
//! multi-firmware is simply multiple `FirmwareMember`s, each flushing/sampling
//! through its own backend, all peers in one engine.

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
    /// Wiring changed since the last validation → revalidate at the next `step`.
    dirty: bool,
    /// Cached invalid-wiring verdict (`None` = valid); re-raised each `step` until
    /// the wiring is fixed (a mutation clears it via `dirty`).
    verdict: Option<RouteError>,
    /// Cached enabled zero-latency route order (topological), rebuilt on validate.
    zl_order: Vec<usize>,
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
            dirty: true,
            verdict: None,
            zl_order: Vec::new(),
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
    /// registration order (deterministic, D7) — an explicit design surface for
    /// forward flow (see the module "Step-time validation" note). Marks the wiring
    /// dirty.
    pub fn add_member(&mut self, mut member: Box<dyn Member + 'b>) {
        member.set_enabled(true, &mut self.state);
        self.members.push(MemberEntry {
            member,
            enabled: true,
        });
        self.dirty = true;
    }

    /// Enable or disable a member by name. A disabled member's advance is skipped
    /// each step (its signals hold their last value); re-enabling calls
    /// [`Member::set_enabled(true)`](Member::set_enabled) again (idempotently
    /// re-registering its signals). Returns whether a member of that name was found;
    /// marks the wiring dirty when it was.
    pub fn set_member_enabled(&mut self, name: &str, on: bool) -> bool {
        if let Some(entry) = self.members.iter_mut().find(|e| e.member.name() == name) {
            entry.enabled = on;
            entry.member.set_enabled(on, &mut self.state);
            self.dirty = true;
            true
        } else {
            false
        }
    }

    /// Add a **zero-latency** `src → dst` route (forward dataflow, same-tick).
    /// Delegates to [`RouteTable::add`]; marks the wiring dirty.
    pub fn add_route(&mut self, src: SignalId, dst: SignalId) -> Result<(), EngineError> {
        self.routes.add(src, dst)?;
        self.dirty = true;
        Ok(())
    }

    /// Add a **delayed** (latency-1) `src → dst` route: the destination receives the
    /// source's *previous-tick* value — the explicit ZOH cut that breaks a feedback
    /// loop. Delegates to [`RouteTable::add_with_latency`]; marks the wiring dirty.
    pub fn add_delayed_route(&mut self, src: SignalId, dst: SignalId) -> Result<(), EngineError> {
        self.routes.add_with_latency(src, dst, 1)?;
        self.dirty = true;
        Ok(())
    }

    /// Suspend a route (stop driving its destination) for fault injection. Marks the
    /// wiring dirty.
    pub fn suspend_route(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), EngineError> {
        self.routes.suspend(src, dst)?;
        self.dirty = true;
        Ok(())
    }

    /// Resume a suspended route. Marks the wiring dirty.
    pub fn resume_route(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), EngineError> {
        self.routes.resume(src, dst)?;
        self.dirty = true;
        Ok(())
    }

    /// Remove a route by its endpoints (delegates to [`RouteTable::remove`]). Marks
    /// the wiring dirty.
    pub fn remove_route(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), EngineError> {
        self.routes.remove(src, dst)?;
        self.dirty = true;
        Ok(())
    }

    /// Advance the whole system one tick (the canonical order — see the module
    /// docs). Each call moves sim time forward by one period. Returns
    /// [`EngineError::Route`] if the wiring is invalid (raised at this step, and
    /// re-raised each step until fixed).
    pub fn step(&mut self) -> Result<(), EngineError> {
        // 1. Sim time advances (monotonic, deterministic — no wall-clock, D7/D9).
        self.now_us += self.tick_period_us;
        self.state.set_time(self.now_us);

        // 2. Validate the wiring if dirty; cache the verdict + zero-latency order.
        if self.dirty {
            let names: Vec<&str> = self.members.iter().map(|e| e.member.name()).collect();
            match self.routes.validate(&names) {
                Ok(order) => {
                    self.zl_order = order;
                    self.verdict = None;
                }
                Err(e) => self.verdict = Some(e),
            }
            self.dirty = false;
        }
        if let Some(e) = &self.verdict {
            return Err(EngineError::Route(e.clone()));
        }

        // 3. Delayed routes: previous-tick values, before any member advances.
        self.routes.propagate_delayed(&mut self.state)?;

        // 4. Each enabled member, in registration order: re-resolve the zero-latency
        //    DAG (topo order, fresh reads), then advance. `routes`, `state`, and
        //    `zl_order` are disjoint from `members`, so these borrows coexist with
        //    the iterator. Propagation is table-only; the member syncs its own
        //    firmware mirrors.
        let dt = self.tick_period_us;
        for entry in &mut self.members {
            if !entry.enabled {
                continue;
            }
            self.routes
                .propagate_zero_latency(&mut self.state, &self.zl_order)?;
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

    /// A model with one input `in` and one output `out = in + step` (both `vsig`s).
    /// Used to build genuine feedback loops through the route table.
    struct AdderModel {
        name: String,
        step: u32,
        out: u32,
    }

    impl AdderModel {
        fn new(name: &str, step: u32) -> Self {
            Self {
                name: name.to_string(),
                step,
                out: 0,
            }
        }
        fn in_id(&self) -> SignalId {
            vsig_id(&self.name, "in").unwrap()
        }
        fn out_id(&self) -> SignalId {
            vsig_id(&self.name, "out").unwrap()
        }
    }

    impl Member for AdderModel {
        fn name(&self) -> &str {
            &self.name
        }
        fn advance(&mut self, _dt_us: u64, st: &mut StateTable) {
            let input = match st.current_value(&self.in_id()).ok().flatten() {
                Some(Value::U32(x)) => *x,
                _ => 0,
            };
            self.out = input.wrapping_add(self.step);
            let _ = st.record(&self.out_id(), Value::U32(self.out));
        }
        fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
            if on {
                let _ = st.register(self.in_id(), None);
                let _ = st.register(self.out_id(), None);
            }
        }
    }

    fn cvar(name: &str) -> SignalId {
        SignalId::new("cvar", "test", name, None).unwrap()
    }

    #[test]
    fn step_flushes_driven_dest_before_advance_tick() {
        // A model member's output routed to a firmware member's DRIVEN cvar must
        // reach firmware memory BEFORE its advance_tick in the same step. Member
        // order [model, firmware] makes it hold — the model records its vsig, the
        // firmware member's pre-advance zero-latency pass records it into the cvar
        // entry, and the firmware member flushes it.
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
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(&Value::U32(3)));
        assert_eq!(eng.state().changes(&id).unwrap().len(), 3);
    }

    #[test]
    fn records_model_vsig_each_tick() {
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(RampModel::new("ramp", 1000.0, Some("counts"))));
        let id = vsig_id("ramp", "value").unwrap();

        assert_eq!(eng.state().current_value(&id).unwrap(), None);
        for _ in 0..5 {
            eng.step().unwrap();
        }
        assert_eq!(eng.state().changes(&id).unwrap().len(), 5);
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(&Value::F64(5.0)));
        assert_eq!(eng.state().value_at(&id, 2_500).unwrap(), Some(&Value::F64(2.0)));
    }

    #[test]
    fn empty_step_advances_firmware_and_time() {
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

        assert!(!eng.set_member_enabled("nope", false));
    }

    #[test]
    fn route_source_unregistered_errors() {
        let be = MockBackend::default();
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(FirmwareMember::new("fw", &be, 1_000)));
        eng.add_route(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::Table(TableError::UnknownSignal(_))))
        ));
    }

    #[test]
    fn zero_latency_chain_resolves_in_one_step() {
        // Three cvars wired a→b→c (zero-latency). With one (nop) member to drive the
        // per-member propagation, a pulse at `a` reaches `c` within the SAME step.
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(OrderModel {
            name: "nop".into(),
            log: Rc::clone(&log),
        }));
        eng.state_mut_seed("cvar:test:a", Value::U32(9));
        eng.state_mut_seed("cvar:test:b", Value::U32(0));
        eng.state_mut_seed("cvar:test:c", Value::U32(0));
        eng.add_route(cvar("a"), cvar("b")).unwrap();
        eng.add_route(cvar("b"), cvar("c")).unwrap();

        eng.step().unwrap();
        assert_eq!(eng.state().current_value(&cvar("b")).unwrap(), Some(&Value::U32(9)));
        assert_eq!(eng.state().current_value(&cvar("c")).unwrap(), Some(&Value::U32(9)));
    }

    #[test]
    fn zero_latency_cycle_errors_at_step_then_delayed_fixes_it() {
        // a↔b as two zero-latency routes is an algebraic loop → error at step.
        let mut eng = Engine::new(1_000);
        eng.state_mut_seed("cvar:test:a", Value::U32(1));
        eng.state_mut_seed("cvar:test:b", Value::U32(2));
        eng.add_route(cvar("a"), cvar("b")).unwrap();
        eng.add_route(cvar("b"), cvar("a")).unwrap();
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::Cycle { .. }))
        ));

        // Break the loop: b→a becomes the delayed edge. Next step is clean.
        eng.remove_route(&cvar("b"), &cvar("a")).unwrap();
        eng.add_delayed_route(cvar("b"), cvar("a")).unwrap();
        eng.step().unwrap();
    }

    #[test]
    fn two_member_feedback_loop_converges_deterministically() {
        // Genuine two-member loop: adder A (idx0) → adder B (idx1) zero-latency,
        // and B → A on a DELAYED edge (the ZOH cut). A zero-latency backward edge is
        // rejected; delayed, the loop steps cleanly with a predictable sequence.
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(AdderModel::new("a", 1))); // out = in + 1
        eng.add_member(Box::new(AdderModel::new("b", 10))); // out = in + 10

        let a_in = vsig_id("a", "in").unwrap();
        let a_out = vsig_id("a", "out").unwrap();
        let b_in = vsig_id("b", "in").unwrap();
        let b_out = vsig_id("b", "out").unwrap();

        // Forward: a.out → b.in (zero-latency, a before b: OK).
        eng.add_route(a_out.clone(), b_in.clone()).unwrap();
        // Backward as zero-latency: b.out → a.in → rejected.
        eng.add_route(b_out.clone(), a_in.clone()).unwrap();
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::BackwardRoute { .. }))
        ));

        // Fix live: make the backward edge delayed.
        eng.remove_route(&b_out, &a_in).unwrap();
        eng.add_delayed_route(b_out.clone(), a_in.clone()).unwrap();

        // Trace: let A_in(t) be a.in at tick t (from b.out at t-1).
        // tick1: a.in=0 → a.out=1; b.in=1 → b.out=11.
        // tick2: a.in=11 (b.out@1) → a.out=12; b.in=12 → b.out=22.
        // tick3: a.in=22 → a.out=23; b.in=23 → b.out=33.
        let expect = [(1u32, 11u32), (12, 22), (23, 33)];
        for (i, (ea, eb)) in expect.iter().enumerate() {
            eng.step().unwrap();
            let ao = match eng.state().current_value(&a_out).unwrap() {
                Some(Value::U32(x)) => *x,
                _ => 0,
            };
            let bo = match eng.state().current_value(&b_out).unwrap() {
                Some(Value::U32(x)) => *x,
                _ => 0,
            };
            assert_eq!((ao, bo), (*ea, *eb), "tick {}", i + 1);
        }
    }

    #[test]
    fn multi_driver_errors_at_step_unless_suspended() {
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(OrderModel {
            name: "nop".into(),
            log,
        })); // drives the per-member zero-latency pass
        eng.state_mut_seed("cvar:test:x", Value::U32(1));
        eng.state_mut_seed("cvar:test:y", Value::U32(2));
        eng.state_mut_seed("cvar:test:dst", Value::U32(0));
        eng.add_route(cvar("x"), cvar("dst")).unwrap();
        eng.add_route(cvar("y"), cvar("dst")).unwrap();
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::MultiDriver { .. }))
        ));

        // Suspend the second driver → the fault-injection swap is legal.
        eng.suspend_route(&cvar("y"), &cvar("dst")).unwrap();
        eng.step().unwrap();
        assert_eq!(eng.state().current_value(&cvar("dst")).unwrap(), Some(&Value::U32(1)));
    }

    #[test]
    fn dirty_flag_caches_verdict_and_revalidates_only_on_change() {
        // Invalid wiring errors at step; the same error is re-raised until fixed;
        // removing the offending route lets the next step pass — no rebuild.
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(1_000);
        eng.add_member(Box::new(OrderModel {
            name: "nop".into(),
            log,
        })); // drives the per-member zero-latency pass
        eng.state_mut_seed("cvar:test:a", Value::U32(1));
        eng.state_mut_seed("cvar:test:b", Value::U32(2));
        eng.add_route(cvar("a"), cvar("b")).unwrap();
        eng.add_route(cvar("b"), cvar("a")).unwrap();

        // Cached invalid verdict, re-raised across steps without a wiring change.
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::Cycle { .. }))
        ));
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::Cycle { .. }))
        ));

        // Fix the wiring live → next step passes.
        eng.remove_route(&cvar("b"), &cvar("a")).unwrap();
        eng.step().unwrap();
        assert_eq!(eng.state().current_value(&cvar("b")).unwrap(), Some(&Value::U32(1)));
    }

    // --- test-only helpers on the engine ---------------------------------

    impl Engine<'_> {
        /// Register + seed a signal directly on the engine's table (test scaffolding
        /// for wiring routes without a driving member).
        fn state_mut_seed(&mut self, id: &str, v: Value) {
            let id = SignalId::parse(id).unwrap();
            let _ = self.state.register(id.clone(), None);
            self.state.force_record(&id, v).unwrap();
        }
    }
}
