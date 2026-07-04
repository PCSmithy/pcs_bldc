//! The Route Table: declarative `source → destination` transport, propagated
//! once per tick, with per-route suspend/resume.
//!
//! A [`Route`] copies one State Table value to another every tick; the list of
//! routes is the system's signal-flow wiring (see
//! `docs/sil/state-route-tables.md` §2). Routes are **pure typed transport** —
//! they copy a value, never transform it; any conversion (amps→counts, angle→SPI
//! frame) belongs in a [`Model`](crate::model::Model), not a route.
//!
//! ## Snapshot-then-write (one delta cycle per tick)
//!
//! [`RouteTable::propagate`] runs in two phases: it first **snapshots every
//! enabled route's source** from the State Table's current cache, then **writes
//! every destination**. Because no source is re-read after any write, route
//! order in the table never changes the result and a chain `x→y→z` advances
//! exactly one hop per tick — the deterministic one-step sampling delay the
//! design wants (state-route-tables.md §3).
//!
//! Sources are read uniformly from the State Table (a `vsig` freshly recorded by
//! the model step, a `cvar` holding the previous tick's firmware output).
//! Destinations are driven through the [`Backend`]: today only `cvar`
//! destinations are wired (by DWARF path — the `name` segment of the id), which
//! is the Phase-3 shape *model output → firmware sensor input*. Writing a model
//! input (`vsig` destination) needs a `Model::write` seam and lands with that
//! chunk; such a destination is rejected at [`add`](RouteTable::add) for now.
//!
//! ## Suspend / resume (fault injection)
//!
//! Each route has an `enabled` flag. [`suspend`](RouteTable::suspend) cuts a
//! route so its destination stops being driven; a test then writes that
//! destination directly (or pins it via [`StateTable::set_override`]) to inject a
//! fault, and [`resume`](RouteTable::resume) restores normal driving. A suspended
//! route is simply skipped by `propagate`.
//!
//! [`StateTable::set_override`]: crate::state_table::StateTable::set_override

use crate::backend::Backend;
use crate::signal::{SignalId, Value};
use crate::state_table::{StateTable, TableError};
use thiserror::Error;

/// One route: copy `src` → `dst` each tick, unless suspended (`enabled == false`).
#[derive(Debug, Clone)]
struct Route {
    src: SignalId,
    dst: SignalId,
    enabled: bool,
}

/// Errors from route authoring and propagation.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum RouteError {
    #[error("duplicate route: {src} -> {dst}")]
    DuplicateRoute { src: SignalId, dst: SignalId },
    #[error("no such route: {src} -> {dst}")]
    UnknownRoute { src: SignalId, dst: SignalId },
    #[error("unsupported route destination {0}: only `cvar` destinations are driven for now")]
    UnsupportedDest(SignalId),
    #[error("route source: {0}")]
    Table(#[from] TableError),
}

/// A flat list of routes. Pure data + an explicit [`propagate`](Self::propagate)
/// call, symmetric with how the [`StateTable`] and model glue work — no engine
/// loop here; the driver (sanity suite / tests, later the sim clock) calls
/// `propagate` once per tick.
pub struct RouteTable {
    routes: Vec<Route>,
}

impl Default for RouteTable {
    fn default() -> Self {
        Self::new()
    }
}

impl RouteTable {
    pub fn new() -> Self {
        Self { routes: Vec::new() }
    }

    /// Add a route `src → dst`. The destination must be a `cvar` (the only wired
    /// backing today). Rejects an exact-duplicate `(src, dst)` pair so
    /// [`suspend`](Self::suspend)/[`resume`](Self::resume) address a route
    /// unambiguously by its endpoints.
    pub fn add(&mut self, src: SignalId, dst: SignalId) -> Result<(), RouteError> {
        if dst.sig_type() != "cvar" {
            return Err(RouteError::UnsupportedDest(dst));
        }
        if self.find(&src, &dst).is_some() {
            return Err(RouteError::DuplicateRoute { src, dst });
        }
        self.routes.push(Route {
            src,
            dst,
            enabled: true,
        });
        Ok(())
    }

    /// Suspend a route (stop driving its destination) by its endpoints.
    pub fn suspend(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), RouteError> {
        self.set_enabled(src, dst, false)
    }

    /// Resume a suspended route.
    pub fn resume(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), RouteError> {
        self.set_enabled(src, dst, true)
    }

    /// Propagate every enabled route once: **snapshot all sources** from the
    /// State Table's current cache, **then write all destinations** via the
    /// backend. A source that has never been recorded (no current value) is
    /// skipped this tick; an unregistered source is a wiring bug and errors.
    pub fn propagate(&self, st: &StateTable, backend: &dyn Backend) -> Result<(), RouteError> {
        // Phase 1 — snapshot every enabled source into a pending write list.
        // Nothing is written yet, so no source can observe a same-tick write.
        let mut pending: Vec<(&SignalId, Value)> = Vec::with_capacity(self.routes.len());
        for route in self.routes.iter().filter(|r| r.enabled) {
            if let Some(v) = st.current_value(&route.src)? {
                pending.push((&route.dst, v.clone()));
            }
        }
        // Phase 2 — drive every destination. `add` guarantees `cvar` dests, whose
        // DWARF path is the id's `name` segment.
        for (dst, value) in pending {
            backend.write_cvar(dst.name(), &value);
        }
        Ok(())
    }

    /// Whether a route exists and is enabled; `None` if there is no such route.
    pub fn is_enabled(&self, src: &SignalId, dst: &SignalId) -> Option<bool> {
        self.find(src, dst).map(|i| self.routes[i].enabled)
    }

    pub fn len(&self) -> usize {
        self.routes.len()
    }
    pub fn is_empty(&self) -> bool {
        self.routes.is_empty()
    }

    // --- internals -------------------------------------------------------

    fn find(&self, src: &SignalId, dst: &SignalId) -> Option<usize> {
        self.routes
            .iter()
            .position(|r| (&r.src == src) && (&r.dst == dst))
    }

    fn set_enabled(&mut self, src: &SignalId, dst: &SignalId, on: bool) -> Result<(), RouteError> {
        match self.find(src, dst) {
            Some(i) => {
                self.routes[i].enabled = on;
                Ok(())
            }
            None => Err(RouteError::UnknownRoute {
                src: src.clone(),
                dst: dst.clone(),
            }),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{record_model, register_model, vsig_id, Model, RampModel};
    use std::cell::RefCell;
    use std::collections::HashMap;

    /// A pure-Rust [`Backend`] whose `cvar` store is a `HashMap`, for exercising
    /// route propagation without a firmware DLL. Unlike [`Firmware`], it applies
    /// no type coercion — it stores whatever [`Value`] a route copies in.
    ///
    /// [`Firmware`]: crate::backend::Firmware
    #[derive(Default)]
    struct MockBackend {
        cvars: RefCell<HashMap<String, Value>>,
    }

    impl MockBackend {
        fn get(&self, path: &str) -> Option<Value> {
            self.cvars.borrow().get(path).cloned()
        }
    }

    impl Backend for MockBackend {
        fn start(&self) -> bool {
            true
        }
        fn advance_tick(&self) {}
        fn shutdown(&self) {}
        fn read_cvar(&self, path: &str) -> Value {
            self.get(path).unwrap_or(Value::U32(0))
        }
        fn write_cvar(&self, path: &str, v: &Value) {
            self.cvars.borrow_mut().insert(path.to_string(), v.clone());
        }
    }

    fn cvar(name: &str) -> SignalId {
        SignalId::new("cvar", "pcs_bldc", name, None).unwrap()
    }

    fn register_cvar(st: &mut StateTable, name: &str, v: Value) -> SignalId {
        let id = cvar(name);
        st.register(id.clone(), None).unwrap();
        st.force_record(&id, v).unwrap();
        id
    }

    #[test]
    fn add_rejects_duplicate_and_non_cvar_dest() {
        let mut rt = RouteTable::new();
        rt.add(cvar("a"), cvar("b")).unwrap();
        assert_eq!(rt.len(), 1);
        // Exact-duplicate endpoints rejected.
        assert!(matches!(
            rt.add(cvar("a"), cvar("b")),
            Err(RouteError::DuplicateRoute { .. })
        ));
        // A `vsig` destination is not wired yet.
        let vsig_dst = vsig_id("motor", "i_a").unwrap();
        assert!(matches!(
            rt.add(cvar("a"), vsig_dst),
            Err(RouteError::UnsupportedDest(_))
        ));
    }

    #[test]
    fn propagate_copies_source_to_cvar_dest() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(7));
        let dst = cvar("dst");

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();

        let be = MockBackend::default();
        rt.propagate(&st, &be).unwrap();
        assert_eq!(be.get("dst"), Some(Value::U32(7)));
    }

    #[test]
    fn suspend_and_resume_gate_propagation() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(1));
        let dst = cvar("dst");
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();

        let be = MockBackend::default();

        // Enabled → drives.
        rt.propagate(&st, &be).unwrap();
        assert_eq!(be.get("dst"), Some(Value::U32(1)));

        // Suspended → destination stops being driven even as the source changes.
        rt.suspend(&src, &dst).unwrap();
        assert_eq!(rt.is_enabled(&src, &dst), Some(false));
        st.force_record(&src, Value::U32(2)).unwrap();
        rt.propagate(&st, &be).unwrap();
        assert_eq!(be.get("dst"), Some(Value::U32(1))); // held at pre-suspend value

        // Resumed → drives again from the source's current value.
        rt.resume(&src, &dst).unwrap();
        assert_eq!(rt.is_enabled(&src, &dst), Some(true));
        rt.propagate(&st, &be).unwrap();
        assert_eq!(be.get("dst"), Some(Value::U32(2)));
    }

    #[test]
    fn suspend_resume_unknown_route_errors() {
        let mut rt = RouteTable::new();
        assert!(matches!(
            rt.suspend(&cvar("a"), &cvar("b")),
            Err(RouteError::UnknownRoute { .. })
        ));
    }

    #[test]
    fn propagate_skips_unset_source_but_errors_on_unregistered() {
        // Registered-but-never-recorded source: nothing to copy, silently skipped.
        let mut st = StateTable::new();
        let src = cvar("src");
        st.register(src.clone(), None).unwrap();
        let dst = cvar("dst");
        let mut rt = RouteTable::new();
        rt.add(src, dst).unwrap();
        let be = MockBackend::default();
        rt.propagate(&st, &be).unwrap();
        assert_eq!(be.get("dst"), None);

        // Unregistered source: a wiring bug → error.
        let st2 = StateTable::new();
        let mut rt2 = RouteTable::new();
        rt2.add(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            rt2.propagate(&st2, &be),
            Err(RouteError::Table(TableError::UnknownSignal(_)))
        ));
    }

    #[test]
    fn chain_snapshot_then_write_uses_pre_tick_values() {
        // a→b, b→c in ONE pass: c must receive the PRE-tick b, not the b that
        // a→b writes this pass (proves snapshot precedes all writes).
        let mut st = StateTable::new();
        let a = register_cvar(&mut st, "a", Value::U32(1));
        let b = register_cvar(&mut st, "b", Value::U32(2));
        let _c = cvar("c");

        let mut rt = RouteTable::new();
        rt.add(a, cvar("b")).unwrap();
        rt.add(b, cvar("c")).unwrap();

        let be = MockBackend::default();
        rt.propagate(&st, &be).unwrap();
        assert_eq!(be.get("b"), Some(Value::U32(1))); // b := a (pre-tick 1)
        assert_eq!(be.get("c"), Some(Value::U32(2))); // c := b (pre-tick 2), NOT 1
    }

    #[test]
    fn chain_advances_one_hop_per_tick() {
        // Drive the engine shape: propagate, then "record" the written dests back
        // into the State Table (the tick-4 historian step). A pulse at `a`
        // marches a→b→c one hop per tick.
        let mut st = StateTable::new();
        let a = register_cvar(&mut st, "a", Value::U32(9));
        let b = register_cvar(&mut st, "b", Value::U32(0));
        let c = register_cvar(&mut st, "c", Value::U32(0));

        let mut rt = RouteTable::new();
        rt.add(a.clone(), b.clone()).unwrap();
        rt.add(b.clone(), c.clone()).unwrap();

        let be = MockBackend::default();
        let commit = |st: &mut StateTable, be: &MockBackend, t: u64| {
            st.set_time(t);
            for name in ["b", "c"] {
                if let Some(v) = be.get(name) {
                    st.record(&cvar(name), v).unwrap();
                }
            }
        };

        // Tick 1: b gets 9, c still 0 (used pre-tick b=0).
        rt.propagate(&st, &be).unwrap();
        commit(&mut st, &be, 1_000);
        assert_eq!(st.current_value(&b).unwrap(), Some(&Value::U32(9)));
        assert_eq!(st.current_value(&c).unwrap(), Some(&Value::U32(0)));

        // Tick 2: the pulse reaches c.
        rt.propagate(&st, &be).unwrap();
        commit(&mut st, &be, 2_000);
        assert_eq!(st.current_value(&c).unwrap(), Some(&Value::U32(9)));
    }

    #[test]
    fn ramp_model_vsig_routes_into_backend() {
        // The Phase-3 shape in miniature: a model's vsig drives a cvar. Advance +
        // record the model, then propagate; the destination tracks it each tick.
        let mut st = StateTable::new();
        let mut model = RampModel::new("ramp", 1000.0, Some("counts")); // +1.0 / ms
        register_model(&mut st, &model).unwrap();
        let src = vsig_id("ramp", "value").unwrap();
        let dst = cvar("sensor_in");

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();
        let be = MockBackend::default();

        for tick in 1..=3u64 {
            model.advance(1_000);
            st.set_time(tick * 1_000);
            record_model(&mut st, &model).unwrap();
            rt.propagate(&st, &be).unwrap();
            assert_eq!(be.get("sensor_in"), Some(Value::F64(tick as f64)));
        }
    }
}
