//! The Route Table: declarative `source → destination` transport, propagated
//! once per tick, with per-route suspend/resume.
//!
//! A [`Route`] copies one State Table value to another every tick; the list of
//! routes is the system's signal-flow wiring (see
//! `docs/sil/state-route-tables.md` §2). Routes are **pure typed transport** —
//! they copy a value, never transform it; any conversion (amps→counts, angle→SPI
//! frame) belongs in a model [`Member`](crate::member::Member), not a route.
//!
//! ## Table-mediated (routes never touch a backend)
//!
//! [`propagate`](RouteTable::propagate) is a **pure State Table operation**: it
//! reads source *entries* and writes destination *entries*, and that is all. It
//! takes **no [`Backend`]** — a route moves a value between two table entries and
//! nothing else. This mirrors the backing model: a `cvar` entry is the table's
//! *mirror* of firmware memory, and a `vsig` entry *abides* in the framework, so
//! moving values between entries is always a table-only act. Members then sync
//! their own mirrors on their own clock — a
//! [`FirmwareMember`](crate::backend::FirmwareMember) flushes its *driven* cvar
//! entries into firmware memory (and samples its *sampled* ones back out) around
//! its `advance_tick`; a model reads a routed `vsig` input via
//! [`StateTable::current_value`]. The route is indifferent to which.
//!
//! Because a destination is written via [`StateTable::record`], it participates in
//! the historian *and* honours [`StateTable::set_override`]: pinning a destination
//! entry makes `record` a no-op, so a route cannot drive it — **fault injection
//! composes with routing at zero extra mechanism.**
//!
//! ## Any registered destination
//!
//! A destination is **any registered signal of any `sig_type`** — a `cvar` (model
//! output → firmware sensor input, flushed by the consuming firmware member) or a
//! `vsig` (a model input, read by the consuming model in its advance). There is no
//! per-`sig_type` restriction: the table is a flat, member-agnostic registry.
//!
//! ## No direction metadata (deliberate)
//!
//! Signals carry no in/out direction, and [`add`](RouteTable::add) does no
//! port-direction checking — the owner rejected that as premature. `add` is
//! **permissive**: a route may be added before its endpoints are registered (or
//! removed at any time). It is the sim designer's job to keep the wiring valid;
//! the framework's job is to **fail loudly, not prevent** — [`propagate`] is the
//! checkpoint (below). If wiring mistakes across port-like signals start to bite,
//! the lever is to add direction metadata on signals plus an `add_route`
//! compatibility check here.
//!
//! ## Snapshot-then-write (one delta cycle per tick)
//!
//! [`propagate`](RouteTable::propagate) runs in two phases: it first **snapshots
//! every enabled route's source** from the State Table's current cache, then
//! **records every destination**. Because no source is re-read after any write,
//! route order in the table never changes the result and a chain `x→y→z` advances
//! exactly one hop per tick — the deterministic one-step sampling delay the
//! design wants (state-route-tables.md §3).
//!
//! Both endpoints must be **registered** at propagate time (symmetric): an
//! unregistered *source* or *destination* is a wiring bug and errors
//! ([`RouteError::Table`]). A registered-but-never-recorded source has nothing to
//! copy and is silently skipped that tick.
//!
//! ## Suspend / resume (fault injection)
//!
//! Each route has an `enabled` flag. [`suspend`](RouteTable::suspend) cuts a
//! route so its destination stops being driven; a test then writes that
//! destination directly (or pins it via [`StateTable::set_override`]) to inject a
//! fault, and [`resume`](RouteTable::resume) restores normal driving. A suspended
//! route is simply skipped by `propagate`.
//!
//! [`Backend`]: crate::backend::Backend
//! [`StateTable::set_override`]: crate::state_table::StateTable::set_override
//! [`StateTable::current_value`]: crate::state_table::StateTable::current_value

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
    #[error("route endpoint: {0}")]
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

    /// Add a route `src → dst`. **Permissive**: the endpoints need not be
    /// registered yet (a route may be authored before its signals exist; validity
    /// is checked at [`propagate`](Self::propagate), not here) and a destination
    /// may be any `sig_type`. Rejects only an exact-duplicate `(src, dst)` pair, so
    /// [`suspend`](Self::suspend)/[`resume`](Self::resume)/[`remove`](Self::remove)
    /// address a route unambiguously by its endpoints.
    pub fn add(&mut self, src: SignalId, dst: SignalId) -> Result<(), RouteError> {
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

    /// Remove a route by its endpoints. Errors [`RouteError::UnknownRoute`] if no
    /// such route exists. Like [`add`](Self::add), removal is legal at any time —
    /// keeping the wiring valid is the sim designer's job.
    pub fn remove(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), RouteError> {
        match self.find(src, dst) {
            Some(i) => {
                self.routes.remove(i);
                Ok(())
            }
            None => Err(RouteError::UnknownRoute {
                src: src.clone(),
                dst: dst.clone(),
            }),
        }
    }

    /// Suspend a route (stop driving its destination) by its endpoints.
    pub fn suspend(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), RouteError> {
        self.set_enabled(src, dst, false)
    }

    /// Resume a suspended route.
    pub fn resume(&mut self, src: &SignalId, dst: &SignalId) -> Result<(), RouteError> {
        self.set_enabled(src, dst, true)
    }

    /// Propagate every enabled route once, **purely on the State Table**:
    /// **snapshot all sources** from the current cache, **then record all
    /// destinations**. Nothing touches any backend — a route moves a value between
    /// two table entries (a `cvar` mirror or a `vsig`); members flush/sample their
    /// own mirrors on their own clock.
    ///
    /// Both endpoints must be registered (symmetric): an unregistered source *or*
    /// destination is a wiring bug and errors. A registered-but-never-recorded
    /// source has no value to copy and is silently skipped this tick. A destination
    /// pinned via [`StateTable::set_override`] absorbs the write as a no-op (the
    /// record is ignored), which is exactly how a suspended-route fault injection
    /// composes with routing.
    ///
    /// [`StateTable::set_override`]: crate::state_table::StateTable::set_override
    pub fn propagate(&self, st: &mut StateTable) -> Result<(), RouteError> {
        // Phase 1 — snapshot every enabled source into a pending write list, and
        // validate both endpoints are registered (symmetric existence check).
        // Nothing is recorded yet, so no source can observe a same-tick write.
        let mut pending: Vec<(&SignalId, Value)> = Vec::with_capacity(self.routes.len());
        for route in self.routes.iter().filter(|r| r.enabled) {
            let value = st.current_value(&route.src)?.cloned(); // source registered?
            st.current_value(&route.dst)?; // destination registered? (value unused)
            if let Some(v) = value {
                pending.push((&route.dst, v));
            }
        }
        // Phase 2 — record every destination into the table (historian + overrides
        // apply). Endpoints were validated above, so this cannot hit UnknownSignal.
        for (dst, value) in pending {
            st.record(dst, value)?;
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
    use crate::member::{vsig_id, Member, RampModel};

    fn cvar(name: &str) -> SignalId {
        SignalId::new("cvar", "dut", name, None).unwrap()
    }

    /// Register `name` as a `cvar` on `st` and seed it with `v` (a current value a
    /// route can snapshot).
    fn register_cvar(st: &mut StateTable, name: &str, v: Value) -> SignalId {
        let id = cvar(name);
        st.register(id.clone(), None).unwrap();
        st.force_record(&id, v).unwrap();
        id
    }

    /// Register a bare (never-recorded) signal so it can be a route destination.
    fn register_dst(st: &mut StateTable, id: &SignalId) {
        st.register(id.clone(), None).unwrap();
    }

    #[test]
    fn add_rejects_duplicate_but_allows_any_dest() {
        let mut rt = RouteTable::new();
        rt.add(cvar("a"), cvar("b")).unwrap();
        assert_eq!(rt.len(), 1);
        // Exact-duplicate endpoints rejected.
        assert!(matches!(
            rt.add(cvar("a"), cvar("b")),
            Err(RouteError::DuplicateRoute { .. })
        ));
        // A `vsig` destination (a model input) is now a first-class route target.
        let vsig_dst = vsig_id("motor", "i_a").unwrap();
        rt.add(cvar("a"), vsig_dst).unwrap();
        assert_eq!(rt.len(), 2);
    }

    #[test]
    fn remove_deletes_and_errors_when_absent() {
        let mut rt = RouteTable::new();
        let (a, b) = (cvar("a"), cvar("b"));
        rt.add(a.clone(), b.clone()).unwrap();
        assert_eq!(rt.len(), 1);
        rt.remove(&a, &b).unwrap();
        assert!(rt.is_empty());
        // Removing a non-existent route errors.
        assert!(matches!(
            rt.remove(&a, &b),
            Err(RouteError::UnknownRoute { .. })
        ));
    }

    #[test]
    fn propagate_records_source_into_destination_entry() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(7));
        let dst = cvar("dst");
        register_dst(&mut st, &dst);

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();

        st.set_time(1_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(7)));
    }

    #[test]
    fn propagate_drives_a_vsig_destination() {
        // A `vsig` destination (model input) works with zero new seams: the route
        // records straight into the table entry, which the consuming model reads.
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "fw_out", Value::F64(1.5));
        let dst = vsig_id("motor", "load_torque").unwrap();
        register_dst(&mut st, &dst);

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();

        st.set_time(1_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::F64(1.5)));
    }

    #[test]
    fn override_on_destination_pins_it_against_the_route() {
        // The free fault-injection win: a pinned destination absorbs the route's
        // record as a no-op, so the route cannot drive it.
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(1));
        let dst = register_cvar(&mut st, "dst", Value::U32(0));
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();

        st.set_override(&dst, true).unwrap();
        st.set_time(1_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(0))); // pinned

        // Unpin → the route drives it normally again.
        st.set_override(&dst, false).unwrap();
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(1)));
    }

    #[test]
    fn suspend_and_resume_gate_propagation() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(1));
        let dst = cvar("dst");
        register_dst(&mut st, &dst);
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();

        // Enabled → drives.
        st.set_time(1_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(1)));

        // Suspended → destination stops being driven even as the source changes.
        rt.suspend(&src, &dst).unwrap();
        assert_eq!(rt.is_enabled(&src, &dst), Some(false));
        st.set_time(2_000);
        st.force_record(&src, Value::U32(2)).unwrap();
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(1))); // held

        // Resumed → drives again from the source's current value.
        rt.resume(&src, &dst).unwrap();
        st.set_time(3_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(2)));
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
    fn propagate_skips_unset_source_but_errors_on_unregistered_endpoints() {
        // Registered-but-never-recorded source: nothing to copy, silently skipped.
        let mut st = StateTable::new();
        let src = cvar("src");
        st.register(src.clone(), None).unwrap();
        let dst = cvar("dst");
        register_dst(&mut st, &dst);
        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), None);

        // Unregistered SOURCE: a wiring bug → error.
        let mut st2 = StateTable::new();
        register_dst(&mut st2, &cvar("dst"));
        let mut rt2 = RouteTable::new();
        rt2.add(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            rt2.propagate(&mut st2),
            Err(RouteError::Table(TableError::UnknownSignal(_)))
        ));

        // Unregistered DESTINATION: symmetric wiring bug → error.
        let mut st3 = StateTable::new();
        register_cvar(&mut st3, "src", Value::U32(1));
        let mut rt3 = RouteTable::new();
        rt3.add(cvar("src"), cvar("ghost_dst")).unwrap();
        assert!(matches!(
            rt3.propagate(&mut st3),
            Err(RouteError::Table(TableError::UnknownSignal(_)))
        ));
    }

    #[test]
    fn chain_snapshot_then_write_uses_pre_tick_values() {
        // a→b, b→c in ONE pass: c must receive the PRE-tick b, not the b that
        // a→b records this pass (proves snapshot precedes all writes).
        let mut st = StateTable::new();
        let a = register_cvar(&mut st, "a", Value::U32(1));
        let b = register_cvar(&mut st, "b", Value::U32(2));
        let c = register_cvar(&mut st, "c", Value::U32(0));

        let mut rt = RouteTable::new();
        rt.add(a, cvar("b")).unwrap();
        rt.add(b, cvar("c")).unwrap();

        st.set_time(1_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&cvar("b")).unwrap(), Some(&Value::U32(1))); // b := a (1)
        assert_eq!(st.current_value(&c).unwrap(), Some(&Value::U32(2))); // c := pre-tick b (2)
    }

    #[test]
    fn chain_advances_one_hop_per_tick() {
        // Pure table-mediated: propagate records dests into the table, and the
        // next tick's snapshot reads them. A pulse at `a` marches a→b→c one hop
        // per tick.
        let mut st = StateTable::new();
        let a = register_cvar(&mut st, "a", Value::U32(9));
        let b = register_cvar(&mut st, "b", Value::U32(0));
        let c = register_cvar(&mut st, "c", Value::U32(0));

        let mut rt = RouteTable::new();
        rt.add(a.clone(), b.clone()).unwrap();
        rt.add(b.clone(), c.clone()).unwrap();

        // Tick 1: b gets 9, c still 0 (used pre-tick b=0).
        st.set_time(1_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&b).unwrap(), Some(&Value::U32(9)));
        assert_eq!(st.current_value(&c).unwrap(), Some(&Value::U32(0)));

        // Tick 2: the pulse reaches c.
        st.set_time(2_000);
        rt.propagate(&mut st).unwrap();
        assert_eq!(st.current_value(&c).unwrap(), Some(&Value::U32(9)));
    }

    #[test]
    fn ramp_model_vsig_routes_into_a_cvar_entry() {
        // The Phase-3 shape in miniature: a model member's vsig drives a cvar
        // entry (which a firmware member would later flush into memory). set_time
        // then advance (records the vsig), then propagate; the destination entry
        // tracks it each tick.
        let mut st = StateTable::new();
        let mut model = RampModel::new("ramp", 1000.0, Some("counts")); // +1.0 / ms
        model.set_enabled(true, &mut st); // registers vsig:ramp:value
        let src = vsig_id("ramp", "value").unwrap();
        let dst = cvar("sensor_in");
        register_dst(&mut st, &dst);

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();

        for tick in 1..=3u64 {
            st.set_time(tick * 1_000);
            model.advance(1_000, &mut st);
            rt.propagate(&mut st).unwrap();
            assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::F64(tick as f64)));
        }
    }
}
