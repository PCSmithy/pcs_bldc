//! The Route Table: declarative `source → destination` transport with a settled
//! **per-route latency** model — zero-latency (default) forward dataflow plus an
//! explicit one-tick delayed edge for the sample/actuation (ZOH) cut of a feedback
//! loop (see `docs/sil/state-route-tables.md` §3).
//!
//! A [`Route`] copies one State Table value to another; the list of routes is the
//! system's signal-flow wiring (state-route-tables.md §2). Routes are **pure typed
//! transport** — they copy a value, never transform it; any conversion (amps→counts,
//! angle→SPI frame) belongs in a model [`Member`](crate::member::Member), not a
//! route.
//!
//! ## Per-route latency (0 or 1)
//!
//! Each route carries a `latency` in **engine ticks**: `0` (default, via
//! [`add`](RouteTable::add)) or `1` (via [`add_with_latency`](RouteTable::add_with_latency)).
//! The type is a `u32` so higher latencies are *representable* later, but anything
//! `> 1` is rejected today ([`RouteError::UnsupportedLatency`]).
//!
//! - A **zero-latency route** is forward dataflow: it copies the source's value as
//!   produced **this same tick** (fresh reads, resolved in topological order — a
//!   chain `a→b→c` resolves fully in one tick).
//! - A **delayed (latency-1) route** models the real ZOH sample/actuation boundary:
//!   its destination receives the source value **as of the end of the previous
//!   tick**. Discrete sim serializes concurrent physics, so every feedback cycle
//!   needs exactly one tick of separation somewhere; the delayed edge is where that
//!   cut is made explicit and physical.
//!
//! ## Table-mediated (routes never touch a backend)
//!
//! Propagation is a **pure State Table operation**: it reads source *entries* and
//! writes destination *entries*, and that is all. It takes **no [`Backend`]** — a
//! route moves a value between two table entries and nothing else. This mirrors the
//! backing model: a `cvar` entry is the table's *mirror* of firmware memory, and a
//! `vsig` entry *abides* in the framework, so moving values between entries is
//! always a table-only act. Members then sync their own mirrors on their own clock —
//! a [`FirmwareMember`](crate::backend::FirmwareMember) flushes its *driven* cvar
//! entries into firmware memory (and samples its *sampled* ones back out) around its
//! `advance_tick`; a model reads a routed `vsig` input via
//! [`StateTable::current_value`].
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
//! ## Add at runtime; validity is checked at step
//!
//! [`add`](RouteTable::add) is **permissive** about endpoints — a route may be
//! authored before its signals are registered, and removed at any time
//! ([`remove`](RouteTable::remove)); [`suspend`](RouteTable::suspend) /
//! [`resume`](RouteTable::resume) toggle a route without removing it. The framework
//! does not *prevent* invalid wiring; the [`Engine`](crate::engine::Engine)
//! **fails loudly** at the next `step` via [`validate`](RouteTable::validate) (the
//! wiring checkpoint, below) and propagation errors on an unregistered endpoint.
//!
//! ## Validation ([`validate`](RouteTable::validate))
//!
//! Given the members' names in **registration order**, `validate` enforces three
//! rules and returns the zero-latency routes in the topological order propagation
//! needs:
//!
//! 1. **Single driver.** Two *enabled* routes with the same destination is a wiring
//!    bug ([`RouteError::MultiDriver`]). Suspended routes are exempt — a
//!    fault-injection swap (suspend one, resume another) stays legal.
//! 2. **Zero-latency graph acyclic** (enabled zero-latency routes only). A cycle is
//!    an ill-posed algebraic loop ([`RouteError::Cycle`]); break it by declaring one
//!    edge delayed.
//! 3. **Forward flow.** Each signal gets an *availability index*: `own(s)` is the
//!    registration index of the member named by `s`'s `<source>` segment (or `-1`
//!    for a driver-owned signal, available before any member advances). Availability
//!    propagates through the zero-latency DAG in topological order —
//!    `avail(s) = max(own(s), max avail(src) over enabled zero-latency routes into s)`.
//!    For every enabled zero-latency route whose destination is owned by a member,
//!    `avail(src) < ownerIndex(dst)` must hold *strictly* — otherwise the value is
//!    consumed before its producer runs, a feedback/backward edge (a same-member
//!    zero-latency loop is a silent delay). Such a route errors
//!    ([`RouteError::BackwardRoute`]): declare it delayed, or reorder the members.
//!
//! ## Suspend / resume (fault injection)
//!
//! Each route has an `enabled` flag. [`suspend`](RouteTable::suspend) cuts a route so
//! its destination stops being driven; a test then writes that destination directly
//! (or pins it via [`StateTable::set_override`]) to inject a fault, and
//! [`resume`](RouteTable::resume) restores normal driving. A suspended route is
//! simply skipped by propagation and exempt from validation.
//!
//! [`Backend`]: crate::backend::Backend
//! [`StateTable::set_override`]: crate::state_table::StateTable::set_override
//! [`StateTable::current_value`]: crate::state_table::StateTable::current_value

use crate::signal::{SignalId, Value};
use crate::state_table::{StateTable, TableError};
use std::collections::{HashMap, VecDeque};
use thiserror::Error;

/// The only latency currently supported beyond zero (one engine tick — the ZOH
/// sample/actuation cut). The type is `u32` so higher latencies are representable
/// later; `validate`/`add_with_latency` reject anything above this for now.
pub const MAX_LATENCY: u32 = 1;

/// One route: copy `src` → `dst` each tick, unless suspended (`enabled == false`).
/// `latency` is in engine ticks (0 = forward/same-tick, 1 = delayed/previous-tick).
#[derive(Debug, Clone)]
struct Route {
    src: SignalId,
    dst: SignalId,
    latency: u32,
    enabled: bool,
}

/// Errors from route authoring, validation, and propagation.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum RouteError {
    #[error("duplicate route: {src} -> {dst}")]
    DuplicateRoute { src: SignalId, dst: SignalId },
    #[error("no such route: {src} -> {dst}")]
    UnknownRoute { src: SignalId, dst: SignalId },
    #[error("route latency {latency} not yet supported (only 0 or 1)")]
    UnsupportedLatency { latency: u32 },
    #[error("multiple enabled routes drive the same destination: {dst}")]
    MultiDriver { dst: SignalId },
    #[error("zero-latency route cycle through {dst} (an algebraic loop): declare one edge delayed")]
    Cycle { dst: SignalId },
    #[error("route {src} -> {dst} needs latency (feedback/backward edge) or reorder members")]
    BackwardRoute { src: SignalId, dst: SignalId },
    #[error("route endpoint: {0}")]
    Table(#[from] TableError),
}

/// A flat list of routes. Pure data + explicit [`validate`](Self::validate) /
/// propagation calls; the [`Engine`](crate::engine::Engine) owns the step loop and
/// calls these once per tick (validating only when the wiring is dirty).
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

    /// Add a **zero-latency** route `src → dst` (forward dataflow, same-tick).
    /// **Permissive**: the endpoints need not be registered yet and a destination
    /// may be any `sig_type`. Rejects only an exact-duplicate `(src, dst)` pair.
    pub fn add(&mut self, src: SignalId, dst: SignalId) -> Result<(), RouteError> {
        self.add_with_latency(src, dst, 0)
    }

    /// Add a route with an explicit `latency` in engine ticks. `0` is forward
    /// dataflow; `1` is the delayed (previous-tick) ZOH cut. Any `latency > 1` is
    /// [`RouteError::UnsupportedLatency`] for now. Same permissive endpoint /
    /// duplicate rules as [`add`](Self::add).
    pub fn add_with_latency(
        &mut self,
        src: SignalId,
        dst: SignalId,
        latency: u32,
    ) -> Result<(), RouteError> {
        if latency > MAX_LATENCY {
            return Err(RouteError::UnsupportedLatency { latency });
        }
        if self.find(&src, &dst).is_some() {
            return Err(RouteError::DuplicateRoute { src, dst });
        }
        self.routes.push(Route {
            src,
            dst,
            latency,
            enabled: true,
        });
        Ok(())
    }

    /// Remove a route by its endpoints. Errors [`RouteError::UnknownRoute`] if no
    /// such route exists.
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

    /// Validate the wiring against `member_names` (member instance names in
    /// **registration order**) and return the enabled **zero-latency** route indices
    /// in the topological order [`propagate_zero_latency`](Self::propagate_zero_latency)
    /// must use. Enforces single-driver, zero-latency acyclicity, and forward-flow
    /// (see the module docs for the exact rules). Pure over the route graph — it
    /// does not touch the State Table.
    pub fn validate(&self, member_names: &[&str]) -> Result<Vec<usize>, RouteError> {
        self.check_single_driver()?;
        let (topo_rank, order) = self.zero_latency_topo()?;
        self.check_forward_flow(member_names, &topo_rank)?;
        Ok(order)
    }

    /// Propagate every enabled **delayed** (latency-1) route once, **snapshot-then-
    /// write** on the State Table: snapshot all delayed sources (each holding its
    /// end-of-previous-tick value, since this runs before any member advances), then
    /// record all delayed destinations. Called once at tick start.
    ///
    /// Both endpoints must be registered (symmetric); a registered-but-never-recorded
    /// source has nothing to copy and is silently skipped; a pinned destination
    /// absorbs the record as a no-op.
    pub fn propagate_delayed(&self, st: &mut StateTable) -> Result<(), RouteError> {
        let mut pending: Vec<(&SignalId, Value)> = Vec::new();
        for route in self.routes.iter().filter(|r| r.enabled && (r.latency > 0)) {
            let value = st.current_value(&route.src)?.cloned(); // source registered?
            st.current_value(&route.dst)?; // destination registered? (value unused)
            if let Some(v) = value {
                pending.push((&route.dst, v));
            }
        }
        for (dst, value) in pending {
            st.record(dst, value)?;
        }
        Ok(())
    }

    /// Propagate the enabled **zero-latency** routes once, in the topological `order`
    /// returned by [`validate`](Self::validate), with **fresh reads**: each route
    /// reads its source's current value and records its destination immediately, so a
    /// chain `a→b→c` resolves fully in one call (later routes see values produced by
    /// earlier ones this same tick).
    ///
    /// The engine re-runs this before *each* member's advance, so a member always
    /// sees the fully-resolved forward dataflow. (Re-evaluating the whole DAG per
    /// member is `M×R` copies — a flagged perf seam; fine at current scale, see
    /// `docs/sil/performance.md`.) Endpoint-registration and override rules match
    /// [`propagate_delayed`](Self::propagate_delayed).
    pub fn propagate_zero_latency(
        &self,
        st: &mut StateTable,
        order: &[usize],
    ) -> Result<(), RouteError> {
        for &ri in order {
            let route = &self.routes[ri];
            let value = st.current_value(&route.src)?.cloned(); // source registered?
            st.current_value(&route.dst)?; // destination registered?
            if let Some(v) = value {
                st.record(&route.dst, v)?;
            }
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

    /// No two enabled routes (any latency) may drive the same destination.
    fn check_single_driver(&self) -> Result<(), RouteError> {
        let mut seen: HashMap<&SignalId, ()> = HashMap::new();
        for route in self.routes.iter().filter(|r| r.enabled) {
            if seen.insert(&route.dst, ()).is_some() {
                return Err(RouteError::MultiDriver {
                    dst: route.dst.clone(),
                });
            }
        }
        Ok(())
    }

    /// Kahn topo-sort over the enabled zero-latency signal graph. Returns
    /// (`topo_rank` per interned signal, enabled-zero-latency route indices ordered
    /// so a route producing signal S precedes any route consuming S). Errors
    /// [`RouteError::Cycle`] on an algebraic loop.
    fn zero_latency_topo(&self) -> Result<(HashMap<&SignalId, usize>, Vec<usize>), RouteError> {
        // Intern the signals of enabled zero-latency routes into dense node ids.
        let zl: Vec<usize> = self
            .routes
            .iter()
            .enumerate()
            .filter(|(_, r)| r.enabled && (r.latency == 0))
            .map(|(i, _)| i)
            .collect();

        let mut node_of: HashMap<&SignalId, usize> = HashMap::new();
        let mut nodes: Vec<&SignalId> = Vec::new();
        for &ri in &zl {
            let route = &self.routes[ri];
            for s in [&route.src, &route.dst] {
                if !node_of.contains_key(s) {
                    node_of.insert(s, nodes.len());
                    nodes.push(s);
                }
            }
        }

        let n = nodes.len();
        let mut adj: Vec<Vec<usize>> = vec![Vec::new(); n];
        let mut indeg: Vec<usize> = vec![0; n];
        for &ri in &zl {
            let route = &self.routes[ri];
            let (su, du) = (node_of[&route.src], node_of[&route.dst]);
            adj[su].push(du);
            indeg[du] += 1;
        }

        // Kahn's algorithm (deterministic: nodes in route-insertion order).
        let mut queue: VecDeque<usize> = (0..n).filter(|&i| indeg[i] == 0).collect();
        let mut topo_rank: Vec<usize> = vec![usize::MAX; n];
        let mut processed = 0usize;
        while let Some(u) = queue.pop_front() {
            topo_rank[u] = processed;
            processed += 1;
            for &v in &adj[u] {
                indeg[v] -= 1;
                if indeg[v] == 0 {
                    queue.push_back(v);
                }
            }
        }
        if processed != n {
            // A node never reached zero in-degree ⇒ it sits on a cycle.
            let stuck = (0..n).find(|&i| topo_rank[i] == usize::MAX).unwrap();
            return Err(RouteError::Cycle {
                dst: nodes[stuck].clone(),
            });
        }

        // Order routes by the topo rank of their destination: a route whose dst is
        // signal S (rank r) runs before any route whose src is S (rank > r).
        let mut order = zl;
        order.sort_by_key(|&ri| topo_rank[node_of[&self.routes[ri].dst]]);

        // Re-key topo_rank onto signals (avail propagation needs it per signal).
        let ranks: HashMap<&SignalId, usize> =
            nodes.iter().enumerate().map(|(i, &s)| (s, topo_rank[i])).collect();
        Ok((ranks, order))
    }

    /// Forward-flow availability check (module docs, rule 3).
    fn check_forward_flow(
        &self,
        member_names: &[&str],
        topo_rank: &HashMap<&SignalId, usize>,
    ) -> Result<(), RouteError> {
        let own = |s: &SignalId| -> i64 {
            member_names
                .iter()
                .position(|m| *m == s.source())
                .map(|i| i as i64)
                .unwrap_or(-1)
        };

        // Propagate availability through the zero-latency DAG in topo order.
        // `topo_rank` covers exactly the signals in that DAG.
        let mut ordered: Vec<&SignalId> = topo_rank.keys().copied().collect();
        ordered.sort_by_key(|s| topo_rank[s]);
        let mut avail: HashMap<&SignalId, i64> =
            ordered.iter().map(|&s| (s, own(s))).collect();
        for route in self.routes.iter().filter(|r| r.enabled && (r.latency == 0)) {
            let a = avail[&route.src];
            let slot = avail.get_mut(&route.dst).unwrap();
            if a > *slot {
                *slot = a;
            }
        }

        // A zero-latency route into a member-owned destination must source a value
        // available strictly before that member's turn.
        for route in self.routes.iter().filter(|r| r.enabled && (r.latency == 0)) {
            let dst_owner = own(&route.dst);
            if dst_owner >= 0 && (avail[&route.src] >= dst_owner) {
                return Err(RouteError::BackwardRoute {
                    src: route.src.clone(),
                    dst: route.dst.clone(),
                });
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::member::{vsig_id, Member, RampModel};

    fn cvar(name: &str) -> SignalId {
        SignalId::new("cvar", "dut", name, None).unwrap()
    }

    /// Register `name` as a `cvar` on `st` and seed it with `v`.
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

    /// Validate with no members, returning the zero-latency topo order.
    fn plan(rt: &RouteTable) -> Vec<usize> {
        rt.validate(&[]).unwrap()
    }

    #[test]
    fn add_rejects_duplicate_but_allows_any_dest() {
        let mut rt = RouteTable::new();
        rt.add(cvar("a"), cvar("b")).unwrap();
        assert_eq!(rt.len(), 1);
        assert!(matches!(
            rt.add(cvar("a"), cvar("b")),
            Err(RouteError::DuplicateRoute { .. })
        ));
        // A `vsig` destination (a model input) is a first-class route target.
        let vsig_dst = vsig_id("motor", "i_a").unwrap();
        rt.add(cvar("a"), vsig_dst).unwrap();
        assert_eq!(rt.len(), 2);
    }

    #[test]
    fn add_with_latency_validates_the_bound() {
        let mut rt = RouteTable::new();
        rt.add_with_latency(cvar("a"), cvar("b"), 0).unwrap();
        rt.add_with_latency(cvar("a"), cvar("c"), 1).unwrap();
        // latency > 1 is not yet supported.
        assert!(matches!(
            rt.add_with_latency(cvar("a"), cvar("d"), 2),
            Err(RouteError::UnsupportedLatency { latency: 2 })
        ));
        assert_eq!(rt.len(), 2);
    }

    #[test]
    fn remove_deletes_and_errors_when_absent() {
        let mut rt = RouteTable::new();
        let (a, b) = (cvar("a"), cvar("b"));
        rt.add(a.clone(), b.clone()).unwrap();
        rt.remove(&a, &b).unwrap();
        assert!(rt.is_empty());
        assert!(matches!(
            rt.remove(&a, &b),
            Err(RouteError::UnknownRoute { .. })
        ));
    }

    #[test]
    fn zero_latency_records_source_into_destination_entry() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(7));
        let dst = cvar("dst");
        register_dst(&mut st, &dst);

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();
        let order = plan(&rt);

        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &order).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(7)));
    }

    #[test]
    fn zero_latency_drives_a_vsig_destination() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "fw_out", Value::F64(1.5));
        let dst = vsig_id("motor", "load_torque").unwrap();
        register_dst(&mut st, &dst);

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();
        let order = plan(&rt);

        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &order).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::F64(1.5)));
    }

    #[test]
    fn override_on_destination_pins_it_against_the_route() {
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(1));
        let dst = register_cvar(&mut st, "dst", Value::U32(0));
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();
        let order = plan(&rt);

        st.set_override(&dst, true).unwrap();
        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &order).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(0))); // pinned

        st.set_override(&dst, false).unwrap();
        rt.propagate_zero_latency(&mut st, &order).unwrap();
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

        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &plan(&rt)).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(1)));

        // Suspended → excluded from the plan, destination held.
        rt.suspend(&src, &dst).unwrap();
        assert_eq!(rt.is_enabled(&src, &dst), Some(false));
        st.set_time(2_000);
        st.force_record(&src, Value::U32(2)).unwrap();
        rt.propagate_zero_latency(&mut st, &plan(&rt)).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(1))); // held

        // Resumed → drives again from the source's current value.
        rt.resume(&src, &dst).unwrap();
        st.set_time(3_000);
        rt.propagate_zero_latency(&mut st, &plan(&rt)).unwrap();
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
        rt.propagate_zero_latency(&mut st, &plan(&rt)).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), None);

        // Unregistered SOURCE: a wiring bug → error.
        let mut st2 = StateTable::new();
        register_dst(&mut st2, &cvar("dst"));
        let mut rt2 = RouteTable::new();
        rt2.add(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            rt2.propagate_zero_latency(&mut st2, &plan(&rt2)),
            Err(RouteError::Table(TableError::UnknownSignal(_)))
        ));

        // Unregistered DESTINATION: symmetric wiring bug → error.
        let mut st3 = StateTable::new();
        register_cvar(&mut st3, "src", Value::U32(1));
        let mut rt3 = RouteTable::new();
        rt3.add(cvar("src"), cvar("ghost_dst")).unwrap();
        assert!(matches!(
            rt3.propagate_zero_latency(&mut st3, &plan(&rt3)),
            Err(RouteError::Table(TableError::UnknownSignal(_)))
        ));
    }

    #[test]
    fn zero_latency_chain_resolves_same_tick() {
        // a→b, b→c both zero-latency: in ONE propagation pass c must receive a's
        // value (the chain resolves fully this tick — the inverse of the old
        // one-hop-per-tick behaviour).
        let mut st = StateTable::new();
        let a = register_cvar(&mut st, "a", Value::U32(9));
        let b = register_cvar(&mut st, "b", Value::U32(0));
        let c = register_cvar(&mut st, "c", Value::U32(0));

        let mut rt = RouteTable::new();
        rt.add(a, cvar("b")).unwrap();
        rt.add(b, cvar("c")).unwrap();
        // Author the chain out of topo order to prove the sort fixes it.
        let order = plan(&rt);

        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &order).unwrap();
        assert_eq!(st.current_value(&cvar("b")).unwrap(), Some(&Value::U32(9)));
        assert_eq!(st.current_value(&c).unwrap(), Some(&Value::U32(9))); // SAME tick
    }

    #[test]
    fn delayed_route_delivers_previous_tick_value() {
        let mut st = StateTable::new();
        let src = cvar("src");
        st.register(src.clone(), None).unwrap();
        let dst = cvar("dst");
        register_dst(&mut st, &dst);
        let mut rt = RouteTable::new();
        rt.add_with_latency(src.clone(), dst.clone(), 1).unwrap();

        // Tick 1: source produced 10 AFTER the delayed pass ⇒ dst not yet driven.
        st.set_time(1_000);
        rt.propagate_delayed(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), None);
        st.force_record(&src, Value::U32(10)).unwrap();

        // Tick 2: dst receives the tick-1 value (10). Source then becomes 20.
        st.set_time(2_000);
        rt.propagate_delayed(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(10)));
        st.force_record(&src, Value::U32(20)).unwrap();

        // Tick 3: dst receives the tick-2 value (20) — exactly one tick of lag.
        st.set_time(3_000);
        rt.propagate_delayed(&mut st).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::U32(20)));
    }

    #[test]
    fn zero_latency_cycle_rejected_unless_one_edge_delayed() {
        let a = cvar("a");
        let b = cvar("b");
        let mut rt = RouteTable::new();
        rt.add(a.clone(), b.clone()).unwrap();
        rt.add(b.clone(), a.clone()).unwrap();
        // a→b→a is an algebraic loop.
        assert!(matches!(rt.validate(&[]), Err(RouteError::Cycle { .. })));

        // Break it: make b→a the delayed (ZOH) edge.
        rt.remove(&b, &a).unwrap();
        rt.add_with_latency(b, a, 1).unwrap();
        assert!(rt.validate(&[]).is_ok());
    }

    #[test]
    fn backward_zero_latency_route_rejected_unless_delayed() {
        // Members "early" (0) then "late" (1). A zero-latency route from late's
        // output into early's input is a backward edge.
        let members = ["early", "late"];
        let src = vsig_id("late", "out").unwrap();
        let dst = vsig_id("early", "in").unwrap();

        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();
        assert!(matches!(
            rt.validate(&members),
            Err(RouteError::BackwardRoute { .. })
        ));

        // The same edge, delayed, is exactly the legal ZOH cut.
        rt.remove(&src, &dst).unwrap();
        rt.add_with_latency(src, dst, 1).unwrap();
        assert!(rt.validate(&members).is_ok());
    }

    #[test]
    fn same_member_zero_latency_loop_is_a_backward_edge() {
        // A zero-latency route within one member's namespace is a silent delay.
        let members = ["m"];
        let src = vsig_id("m", "out").unwrap();
        let dst = vsig_id("m", "in").unwrap();
        let mut rt = RouteTable::new();
        rt.add(src, dst).unwrap();
        assert!(matches!(
            rt.validate(&members),
            Err(RouteError::BackwardRoute { .. })
        ));
    }

    #[test]
    fn multi_driver_rejected_unless_second_suspended() {
        let dst = cvar("dst");
        let mut rt = RouteTable::new();
        rt.add(cvar("a"), dst.clone()).unwrap();
        rt.add(cvar("b"), dst.clone()).unwrap();
        assert!(matches!(
            rt.validate(&[]),
            Err(RouteError::MultiDriver { .. })
        ));

        // Suspending the second driver makes the swap legal (fault injection).
        rt.suspend(&cvar("b"), &dst).unwrap();
        assert!(rt.validate(&[]).is_ok());
    }

    #[test]
    fn ramp_model_vsig_routes_into_a_cvar_entry() {
        let mut st = StateTable::new();
        let mut model = RampModel::new("ramp", 1000.0, Some("counts")); // +1.0 / ms
        model.set_enabled(true, &mut st);
        let src = vsig_id("ramp", "value").unwrap();
        let dst = cvar("sensor_in");
        register_dst(&mut st, &dst);

        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();
        let order = plan(&rt);

        for tick in 1..=3u64 {
            st.set_time(tick * 1_000);
            model.advance(1_000, &mut st);
            rt.propagate_zero_latency(&mut st, &order).unwrap();
            assert_eq!(st.current_value(&dst).unwrap(), Some(&Value::F64(tick as f64)));
        }
    }
}
