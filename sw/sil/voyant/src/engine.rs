//! The sim clock + step loop: the engine that ties the parts together.
//!
//! [`Engine`] owns the [`StateTable`], the [`RouteTable`], and the plant/peer
//! [`Model`]s, borrows a [`Backend`] (the firmware under test), and advances the
//! whole system one tick at a time with [`step`](Engine::step). It is the first
//! consumer of everything below it — the piece the design docs call the "sim
//! core" (`architecture.md` §5, `state-route-tables.md` §3).
//!
//! ## The canonical tick order
//!
//! Each [`step`](Engine::step) advances sim time by one fixed tick period and runs
//! the deterministic order from `state-route-tables.md` §3:
//!
//! 1. **advance sim time** — `now += tick_period`, monotonic and wall-clock-free
//!    (D7/D9); [`StateTable::set_time`] stamps every record this tick.
//! 2. **advance models** in **registration order**, recording each model's `vsig`
//!    outputs into the State Table (the docs' "model output entries updated").
//! 3. **propagate routes** (snapshot-then-write) — a model output routed to a
//!    firmware `cvar` lands **before** the firmware runs this tick.
//! 4. **`advance_tick`** — the firmware runs to quiescence.
//! 5. **sample** the registered firmware `cvar`s into the State Table historian.
//!
//! Steps 2 and 5 together are the docs' single "record signals" phase, split so a
//! model output is visible to routes in the *same* tick (step 3) while firmware
//! outputs are captured *after* the firmware ran (step 5). Both record at the same
//! `now`, so the historian timestamps stay consistent.
//!
//! Lifecycle (`start`/`shutdown`) stays on the [`Backend`] handle the caller
//! holds — the engine only drives the loop. Run modes / pacing (fast vs realtime)
//! are a thin wrapper over `step` and land in a later chunk; nothing here presumes
//! either.
//!
//! ## Performance shape (see `performance.md`)
//!
//! The hot loop is **allocation-free on the engine's own account**: each model's
//! `vsig` [`SignalId`]s and the sampled-`cvar` list are resolved once at
//! registration and iterated in place, so the per-tick model recording does **not**
//! call [`Model::signals`] (which allocates a `Vec`). The remaining per-tick
//! allocations live behind seams owned elsewhere and flagged for later work: the
//! `pending` snapshot buffer in [`RouteTable::propagate`] (to become flat
//! `(src, dst)` arrays) and any `Enum`/`Bytes` [`Value`] a read materializes (the
//! historian goes columnar later). The engine API takes none of these into its
//! surface, so those optimizations land without an API break.

use crate::backend::Backend;
use crate::model::{vsig_id, Model};
use crate::route::{RouteError, RouteTable};
use crate::signal::{ParseError, SignalId};
use crate::state_table::{StateTable, TableError};
use thiserror::Error;

/// A registered model plus its `vsig` [`SignalId`]s, resolved once so the tick
/// loop never calls [`Model::signals`] (which allocates).
struct ModelEntry {
    model: Box<dyn Model>,
    vsigs: Vec<SignalId>,
}

/// Errors from engine setup and stepping.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum EngineError {
    #[error("signal id: {0}")]
    Id(#[from] ParseError),
    #[error(transparent)]
    Table(#[from] TableError),
    #[error(transparent)]
    Route(#[from] RouteError),
}

/// The sim clock + step loop. Owns the State Table (the historian), the Route
/// Table, and the models; borrows a [`Backend`] for the firmware side.
///
/// The backend is a shared borrow (`&dyn Backend`): every [`Backend`] method
/// takes `&self` (it mutates the firmware's own memory, not the Rust handle), so
/// the caller can keep its own `&Backend` for ad-hoc white-box injection /
/// inspection alongside the engine — the two never contend.
pub struct Engine<'b> {
    backend: &'b dyn Backend,
    state: StateTable,
    routes: RouteTable,
    models: Vec<ModelEntry>,
    /// Firmware `cvar`s sampled into the historian every tick (step 5).
    sampled: Vec<SignalId>,
    tick_period_us: u64,
    now_us: u64,
}

impl<'b> Engine<'b> {
    /// A new engine driving `backend`, advancing `tick_period_us` of sim time per
    /// [`step`](Self::step) (e.g. 1000 for the 1 kHz firmware tick). Sim time
    /// starts at 0; the first `step` records at `tick_period_us`.
    pub fn new(backend: &'b dyn Backend, tick_period_us: u64) -> Self {
        Self {
            backend,
            state: StateTable::new(),
            routes: RouteTable::new(),
            models: Vec::new(),
            sampled: Vec::new(),
            tick_period_us,
            now_us: 0,
        }
    }

    /// A new engine with a caller-configured [`StateTable`] (retention / epsilon).
    pub fn with_state(backend: &'b dyn Backend, tick_period_us: u64, state: StateTable) -> Self {
        Self {
            state,
            ..Self::new(backend, tick_period_us)
        }
    }

    /// Register a model: create its `vsig` State Table entries and add it to the
    /// advance list. Advance order is registration order (deterministic, D7).
    pub fn add_model(&mut self, model: Box<dyn Model>) -> Result<(), EngineError> {
        let mut vsigs = Vec::with_capacity(4);
        for sig in model.signals() {
            let id = vsig_id(model.name(), &sig.local)?;
            self.state.register(id.clone(), sig.unit.as_deref())?;
            vsigs.push(id);
        }
        self.models.push(ModelEntry { model, vsigs });
        Ok(())
    }

    /// Add a `src → dst` route (delegates to the [`RouteTable`]; `dst` must be a
    /// `cvar`, see [`RouteTable::add`]).
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

    /// Register a firmware `cvar` to be sampled into the historian every tick
    /// (its DWARF path is the id's `name` segment). Idempotent per id via the
    /// State Table's duplicate-registration guard.
    pub fn sample_cvar(&mut self, id: SignalId, unit: Option<&str>) -> Result<(), EngineError> {
        self.state.register(id.clone(), unit)?;
        self.sampled.push(id);
        Ok(())
    }

    /// Advance the whole system one tick (the canonical order — see the module
    /// docs). Idempotent-free: each call moves sim time forward by one period.
    pub fn step(&mut self) -> Result<(), EngineError> {
        // 1. Sim time advances (monotonic, deterministic — no wall-clock, D7/D9).
        self.now_us += self.tick_period_us;
        self.state.set_time(self.now_us);

        // 2. Advance every model in registration order and record its vsig
        //    outputs (the "model output entries updated" of the canonical order).
        //    Uses the pre-resolved id list — no per-tick Model::signals() alloc.
        let dt = self.tick_period_us;
        for entry in &mut self.models {
            entry.model.advance(dt);
            for id in &entry.vsigs {
                if let Some(v) = entry.model.read(id.name()) {
                    self.state.record(id, v)?;
                }
            }
        }

        // 3. Propagate routes into firmware inputs — BEFORE the firmware runs.
        self.routes.propagate(&self.state, self.backend)?;

        // 4. Run the firmware to quiescence.
        self.backend.advance_tick();

        // 5. Sample the registered firmware cvars into the historian.
        for id in &self.sampled {
            let v = self.backend.read_cvar(id.name());
            self.state.record(id, v)?;
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Model, ModelSignal, RampModel};
    use crate::signal::Value;
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;
    use std::rc::Rc;

    /// A pure-Rust [`Backend`] that records the order of its calls, so a test can
    /// prove *when* in a step a route write vs `advance_tick` happens. Reads return
    /// a stored value if written, else a `U32` of the tick count (a firmware-less
    /// ramp for the sampling test).
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

    /// A model that appends its name to a shared log when advanced — for proving
    /// multi-model advance order. Exposes no signals.
    struct OrderModel {
        name: String,
        log: Rc<RefCell<Vec<String>>>,
    }

    impl Model for OrderModel {
        fn name(&self) -> &str {
            &self.name
        }
        fn signals(&self) -> Vec<ModelSignal> {
            vec![]
        }
        fn advance(&mut self, _dt_us: u64) {
            self.log.borrow_mut().push(self.name.clone());
        }
        fn read(&self, _local: &str) -> Option<Value> {
            None
        }
    }

    fn cvar(name: &str) -> SignalId {
        SignalId::new("cvar", "test", name, None).unwrap()
    }

    #[test]
    fn step_writes_route_dest_before_advance_tick() {
        // The load-bearing ordering guarantee: a model output routed to a cvar
        // must reach the firmware BEFORE advance_tick of the same step.
        let be = MockBackend::default();
        let mut eng = Engine::new(&be, 1_000);
        eng.add_model(Box::new(RampModel::new("ramp", 1000.0, None)))
            .unwrap();
        eng.add_route(vsig_id("ramp", "value").unwrap(), cvar("sensor_in"))
            .unwrap();

        eng.step().unwrap();

        let log = be.log.borrow();
        let w = log.iter().position(|e| e == "write:sensor_in").unwrap();
        let t = log.iter().position(|e| e == "advance_tick").unwrap();
        assert!(w < t, "route write must precede advance_tick, got {log:?}");
        // And the firmware actually received the model's first-tick value.
        assert_eq!(be.cvars.borrow().get("sensor_in"), Some(&Value::F64(1.0)));
    }

    #[test]
    fn time_advances_by_tick_period() {
        let be = MockBackend::default();
        let mut eng = Engine::new(&be, 1_000);
        let id = cvar("counter");
        eng.sample_cvar(id.clone(), None).unwrap();

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
    fn multi_model_advances_in_registration_order() {
        let be = MockBackend::default();
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(&be, 1_000);
        for name in ["first", "second", "third"] {
            eng.add_model(Box::new(OrderModel {
                name: name.to_string(),
                log: Rc::clone(&log),
            }))
            .unwrap();
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
        let mut eng = Engine::new(&be, 1_000);
        let id = cvar("ramp_counter");
        eng.sample_cvar(id.clone(), Some("counts")).unwrap();

        for _ in 0..3 {
            eng.step().unwrap();
        }
        // Read after advance_tick ⇒ the post-tick counts 1,2,3.
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(&Value::U32(3)));
        assert_eq!(eng.state().changes(&id).unwrap().len(), 3);
    }

    #[test]
    fn records_model_vsig_each_tick() {
        let be = MockBackend::default();
        let mut eng = Engine::new(&be, 1_000);
        eng.add_model(Box::new(RampModel::new("ramp", 1000.0, Some("counts"))))
            .unwrap();
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
        // No models, routes, or sampled cvars: a step is just time + advance_tick.
        let be = MockBackend::default();
        let mut eng = Engine::new(&be, 2_000);
        eng.step().unwrap();
        eng.step().unwrap();
        assert_eq!(eng.now_us(), 4_000);
        assert_eq!(be.ticks.get(), 2);
        assert_eq!(*be.log.borrow(), vec!["advance_tick", "advance_tick"]);
    }

    #[test]
    fn route_source_unregistered_errors() {
        let be = MockBackend::default();
        let mut eng = Engine::new(&be, 1_000);
        // A route from a cvar that is never registered/sampled is a wiring bug.
        eng.add_route(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            eng.step(),
            Err(EngineError::Route(RouteError::Table(TableError::UnknownSignal(_))))
        ));
    }
}
