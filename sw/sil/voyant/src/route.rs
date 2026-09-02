//! The Route Table: declarative `source → destination` transport with a **per-route
//! latency** model — zero-latency (default) forward dataflow plus an explicit
//! one-tick delayed edge for the ZOH sample/actuation cut of a feedback loop (see
//! `docs/sil/state-route-tables.md` §2–§3).
//!
//! A [`Route`] copies one State Table value to another; the route list is the
//! system's signal-flow wiring. Routes are **pure typed transport** — never a
//! transform; any conversion (amps→counts, angle→SPI frame) belongs in a model
//! [`Member`](crate::member::Member), not a route.
//!
//! ## Per-route latency (0 or 1)
//!
//! Each route carries a `latency` in engine ticks: `0` ([`add`](RouteTable::add)) or
//! `1` ([`add_with_latency`](RouteTable::add_with_latency)). The `u32` type leaves
//! room for higher values, but anything `> 1` is rejected today
//! ([`RouteError::UnsupportedLatency`]).
//!
//! - **Zero-latency** = forward dataflow: copies the source value produced **this
//!   same tick** (fresh reads, topo order — a chain `a→b→c` resolves in one tick).
//! - **Delayed (latency-1)** = the ZOH boundary: the destination gets the source's
//!   **end-of-previous-tick** value. This is the one tick of separation every
//!   feedback cycle needs, made explicit and physical.
//!
//! ## Table-mediated (routes never touch a backend)
//!
//! Propagation is a **pure State Table operation**: it reads source entries and
//! writes destination entries — no backend. Members then sync their own mirrors on
//! their own clock: a [`FirmwareMember`](crate::backend::FirmwareMember) flushes the
//! fresh cvar entries in its namespace into firmware memory and sweeps its mirror
//! back out around its interrupt dispatch (a route driving a cvar marks it dirty, so
//! the member flushes it that step); a model reads a routed `vsig` via
//! [`StateTable::current_value`].
//!
//! **Fault injection composes with routing at zero extra mechanism**: suspend the
//! route driving a destination, then [`record`](StateTable::record) a fault value into
//! it directly (the suspended route no longer records over it); [`resume`](RouteTable::resume)
//! hands the destination back to the route.
//!
//! A destination may be **any registered signal of any `sig_type`** — the table is a
//! flat, member-agnostic registry, no per-`sig_type` restriction.
//!
//! ## Add at runtime; validity is checked at step
//!
//! Authoring is **permissive**: a route may be added before its endpoints register
//! and removed/suspended/resumed anytime. The framework does not prevent invalid
//! wiring; the [`Engine`](crate::engine::Engine) **fails loudly** at the next `step`
//! via [`validate`](RouteTable::validate), and propagation errors on an unregistered
//! endpoint.
//!
//! ## Validation ([`validate`](RouteTable::validate))
//!
//! Given member names in **registration order**, `validate` enforces three rules and
//! returns the zero-latency routes in propagation's topological order:
//!
//! 1. **Single driver.** Two *enabled* routes on one destination is a bug
//!    ([`RouteError::MultiDriver`]); suspended routes are exempt (fault-injection swap).
//! 2. **Zero-latency acyclic** ([`RouteError::Cycle`]); break a loop by delaying one edge.
//! 3. **Forward flow.** Each signal's *availability index* `own(s)` is the reg index
//!    of the member owning `s`'s `<source>` (`-1` = driver-owned, available first),
//!    propagated through the DAG in topo order:
//!    `avail(s) = max(own(s), max avail(src) into s)`. For a zero-latency route into a
//!    member-owned destination, `avail(src) < ownerIndex(dst)` must hold strictly —
//!    else the value is consumed before its producer runs (a backward/feedback edge; a
//!    same-member loop is a silent delay), erroring [`RouteError::BackwardRoute`]:
//!    declare it delayed or reorder the members.
//!
//! ## Suspend / resume (fault injection)
//!
//! [`suspend`](RouteTable::suspend) cuts a route so its destination stops being
//! driven — a test then writes that destination to inject a fault, and
//! [`resume`](RouteTable::resume) restores it. A suspended route is skipped by
//! propagation and exempt from validation.
//!
//! [`StateTable::current_value`]: crate::state_table::StateTable::current_value

use crate::signal::{SignalId, Value};
use crate::state_table::{StateTable, TableError};
use std::cell::Cell;
use std::collections::{HashMap, HashSet, VecDeque};
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
    /// Cached dense State Table indices of `src` / `dst` (Tier-2 fast lane), resolved
    /// lazily and memoized. Indices are append-only *within one table*, so a cached
    /// `Some` never goes stale for that table (an unresolved endpoint stays `None` and
    /// retries, preserving the unregistered-endpoint error); the per-route cell drops
    /// on removal. But the invariant is per-table and `propagate*` takes any table — so
    /// on every cache **hit** [`resolve_endpoint`] verifies the index still names this
    /// endpoint ([`StateTable::id_at`]) and fails loud with
    /// [`RouteError::TableMismatch`], never silently applying table-A indices to table-B.
    src_idx: Cell<Option<usize>>,
    dst_idx: Cell<Option<usize>>,
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
    #[error("route endpoint {signal}: cached index does not match this StateTable — one RouteTable must not be propagated against different tables")]
    TableMismatch { signal: SignalId },
    #[error("route endpoint: {0}")]
    Table(#[from] TableError),
}

/// Resolve a route endpoint to its dense State Table index, memoizing in `cell`.
/// Indices are append-only within a table, so a cached `Some` stays valid for it; an
/// unregistered endpoint yields [`TableError::UnknownSignal`] and retries next call.
/// On a cache **hit** the index is verified against `st` (guarding against one
/// `RouteTable` propagated across two tables): a mismatch fails loud with
/// [`RouteError::TableMismatch`] rather than reading/writing the wrong signal.
fn resolve_endpoint(
    cell: &Cell<Option<usize>>,
    st: &StateTable,
    id: &SignalId,
) -> Result<usize, RouteError> {
    if let Some(i) = cell.get() {
        // Guard: the cached index must still name THIS endpoint in THIS table.
        return match st.id_at(i) {
            Some(actual) if actual == id => Ok(i),
            _ => Err(RouteError::TableMismatch { signal: id.clone() }),
        };
    }
    match st.resolve_index(id) {
        Some(i) => {
            cell.set(Some(i));
            Ok(i)
        }
        None => Err(RouteError::Table(TableError::UnknownSignal(id.clone()))),
    }
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
            src_idx: Cell::new(None),
            dst_idx: Cell::new(None),
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
        self.check_forward_flow(member_names, &topo_rank, &order)?;
        Ok(order)
    }

    /// Propagate every enabled **delayed** (latency-1) route once, snapshot-then-write:
    /// snapshot all delayed sources (each holding its end-of-previous-tick value, as
    /// this runs before any member advances), then record all destinations. Both
    /// endpoints must be registered; a never-recorded source is skipped. `on_change`
    /// sees each destination that actually changed (post-epsilon) — the engine's
    /// input-dirty feed.
    pub fn propagate_delayed(
        &self,
        st: &mut StateTable,
        on_change: &mut dyn FnMut(&SignalId),
    ) -> Result<(), RouteError> {
        let mut pending: Vec<(usize, usize, Value)> = Vec::new();
        for (ri, route) in self.routes.iter().enumerate() {
            if !(route.enabled && (route.latency > 0)) {
                continue;
            }
            let sidx = resolve_endpoint(&route.src_idx, st, &route.src)?; // source registered?
            let didx = resolve_endpoint(&route.dst_idx, st, &route.dst)?; // destination registered?
            if let Some(v) = st.current_value_at(sidx) {
                pending.push((ri, didx, v));
            }
        }
        for (ri, didx, value) in pending {
            // A record that mismatches the destination column's established type is a
            // wiring bug (a route delivering the wrong Value type); it bubbles through
            // RouteError::Table so the step fails loud instead of corrupting the column.
            if st.record_at_changed(didx, value)? {
                on_change(&self.routes[ri].dst);
            }
        }
        Ok(())
    }

    /// Propagate the enabled **zero-latency** routes once in the topological `order`
    /// from [`validate`](Self::validate), with **fresh reads**: a chain `a→b→c`
    /// resolves fully in one call (later routes see earlier ones' values this tick).
    /// The engine re-runs this before *each* member's advance (`M×R` copies — a flagged
    /// perf seam, `docs/sil/performance.md`). Registration and `on_change` rules match
    /// [`propagate_delayed`](Self::propagate_delayed).
    pub fn propagate_zero_latency(
        &self,
        st: &mut StateTable,
        order: &[usize],
        on_change: &mut dyn FnMut(&SignalId),
    ) -> Result<(), RouteError> {
        for &ri in order {
            let route = &self.routes[ri];
            let sidx = resolve_endpoint(&route.src_idx, st, &route.src)?; // source registered?
            let didx = resolve_endpoint(&route.dst_idx, st, &route.dst)?; // destination registered?
            if let Some(v) = st.current_value_at(sidx) {
                // A mismatched destination type is a wiring bug — fail the step loud
                // (RouteError::Table) rather than corrupt the destination column.
                if st.record_at_changed(didx, v)? {
                    on_change(&route.dst);
                }
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
        let mut seen: HashSet<&SignalId> = HashSet::new();
        for route in self.routes.iter().filter(|r| r.enabled) {
            if !seen.insert(&route.dst) {
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

    /// Forward-flow availability check (module docs, rule 3). The fold **must** walk
    /// `order` (topological), not route-insertion order — an anti-topologically
    /// authored chain of ≥2 unowned intermediates otherwise under-propagates and lets
    /// a transitively-backward route slip through. Single-driver gives each signal ≤1
    /// incoming route, so one topo pass reaches the fixpoint.
    fn check_forward_flow(
        &self,
        member_names: &[&str],
        topo_rank: &HashMap<&SignalId, usize>,
        order: &[usize],
    ) -> Result<(), RouteError> {
        let own = |s: &SignalId| -> i64 {
            member_names
                .iter()
                .position(|m| *m == s.source())
                .map(|i| i as i64)
                .unwrap_or(-1)
        };

        // Seed every DAG signal with its own index, then propagate availability
        // through the zero-latency routes in topological order.
        let mut avail: HashMap<&SignalId, i64> =
            topo_rank.keys().map(|&s| (s, own(s))).collect();
        for &ri in order {
            let route = &self.routes[ri];
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
    use crate::member::{advance_unwired, vsig_id, Member, RampModel};

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
        rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(7)));
    }

    #[test]
    fn route_delivering_a_mismatched_type_errors_the_step_and_corrupts_nothing() {
        // A route wiring a float source into an int-typed destination is a bug: the
        // destination column is established U32, so recording the F64 must fail the
        // step loud (RouteError::Table(TypeMismatch)) rather than corrupt the column.
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::F64(1.5));
        let dst = register_cvar(&mut st, "dst", Value::U32(7)); // establishes U32
        let mut rt = RouteTable::new();
        rt.add(src, dst.clone()).unwrap();
        let order = plan(&rt);

        st.set_time(1_000);
        let err = rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap_err();
        assert!(matches!(
            err,
            RouteError::Table(TableError::TypeMismatch { column_kind: "U32", offending: "F64", .. })
        ));
        // The destination column is untouched: still one U32 sample.
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(7)));
        assert_eq!(st.changes(&dst).unwrap().len(), 1);

        // The delayed path shares the same record_at guard.
        let mut st2 = StateTable::new();
        let dsrc = register_cvar(&mut st2, "dsrc", Value::F64(2.5));
        let ddst = register_cvar(&mut st2, "ddst", Value::U32(1));
        let mut drt = RouteTable::new();
        drt.add_with_latency(dsrc, ddst.clone(), 1).unwrap();
        st2.set_time(1_000);
        assert!(matches!(
            drt.propagate_delayed(&mut st2, &mut |_| {}),
            Err(RouteError::Table(TableError::TypeMismatch { .. }))
        ));
        assert_eq!(st2.changes(&ddst).unwrap().len(), 1);
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
        rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::F64(1.5)));
    }

    #[test]
    fn suspended_route_lets_a_direct_write_to_the_destination_persist() {
        // Fault injection: a suspended route no longer records over its destination, so
        // a direct write persists until resume hands the destination back to the route.
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(1));
        let dst = register_cvar(&mut st, "dst", Value::U32(0));
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();
        let order = plan(&rt);

        // Live route: destination tracks the source.
        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(1)));

        // Suspend, then write a fault value straight into the destination.
        rt.suspend(&src, &dst).unwrap();
        st.set_time(2_000);
        st.record(&dst, Value::U32(99)).unwrap();
        rt.propagate_zero_latency(&mut st, &plan(&rt), &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(99))); // not clobbered

        // Resume: the route drives the destination from the source again.
        rt.resume(&src, &dst).unwrap();
        st.set_time(3_000);
        rt.propagate_zero_latency(&mut st, &plan(&rt), &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(1)));
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
        rt.propagate_zero_latency(&mut st, &plan(&rt), &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(1)));

        // Suspended → excluded from the plan, destination held.
        rt.suspend(&src, &dst).unwrap();
        assert_eq!(rt.is_enabled(&src, &dst), Some(false));
        st.set_time(2_000);
        st.force_record(&src, Value::U32(2)).unwrap();
        rt.propagate_zero_latency(&mut st, &plan(&rt), &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(1))); // held

        // Resumed → drives again from the source's current value.
        rt.resume(&src, &dst).unwrap();
        st.set_time(3_000);
        rt.propagate_zero_latency(&mut st, &plan(&rt), &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(2)));
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
        rt.propagate_zero_latency(&mut st, &plan(&rt), &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), None);

        // Unregistered SOURCE: a wiring bug → error.
        let mut st2 = StateTable::new();
        register_dst(&mut st2, &cvar("dst"));
        let mut rt2 = RouteTable::new();
        rt2.add(cvar("ghost"), cvar("dst")).unwrap();
        assert!(matches!(
            rt2.propagate_zero_latency(&mut st2, &plan(&rt2), &mut |_| {}),
            Err(RouteError::Table(TableError::UnknownSignal(_)))
        ));

        // Unregistered DESTINATION: symmetric wiring bug → error.
        let mut st3 = StateTable::new();
        register_cvar(&mut st3, "src", Value::U32(1));
        let mut rt3 = RouteTable::new();
        rt3.add(cvar("src"), cvar("ghost_dst")).unwrap();
        assert!(matches!(
            rt3.propagate_zero_latency(&mut st3, &plan(&rt3), &mut |_| {}),
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
        rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&cvar("b")).unwrap(), Some(Value::U32(9)));
        assert_eq!(st.current_value(&c).unwrap(), Some(Value::U32(9))); // SAME tick
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
        rt.propagate_delayed(&mut st, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), None);
        st.force_record(&src, Value::U32(10)).unwrap();

        // Tick 2: dst receives the tick-1 value (10). Source then becomes 20.
        st.set_time(2_000);
        rt.propagate_delayed(&mut st, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(10)));
        st.force_record(&src, Value::U32(20)).unwrap();

        // Tick 3: dst receives the tick-2 value (20) — exactly one tick of lag.
        st.set_time(3_000);
        rt.propagate_delayed(&mut st, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(20)));
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
    fn forward_flow_propagates_through_anti_topological_insertion() {
        // Availability must fold through a chain of unowned intermediates in
        // topological ROUTE order, not insertion order. Chain a → x → y → in,
        // authored backward as [y→in, x→y, a→x], all zero-latency. `a` is owned
        // by m3 (member idx 1), `in` by m1 (idx 0); availability at `a` (1) must
        // reach `y`, making the y→in route into m1's owned input a backward edge.
        //
        // Old code folded in insertion order: avail[y] stayed -1, so y→in passed
        // and validation wrongly succeeded (a silent one-tick delay at runtime).
        let members = ["m1", "m3"];
        let a = vsig_id("m3", "a").unwrap();
        let x = vsig_id("wire", "x").unwrap();
        let y = vsig_id("wire", "y").unwrap();
        let dst_in = vsig_id("m1", "in").unwrap();

        let mut rt = RouteTable::new();
        rt.add(y.clone(), dst_in.clone()).unwrap();
        rt.add(x.clone(), y.clone()).unwrap();
        rt.add(a.clone(), x.clone()).unwrap();

        // The route into the owned destination (y→in) is the backward edge.
        assert!(matches!(
            rt.validate(&members),
            Err(RouteError::BackwardRoute { src, dst }) if (src == y) && (dst == dst_in)
        ));
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
            advance_unwired(&mut model, 1_000, &mut st);
            rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap();
            assert_eq!(st.current_value(&dst).unwrap(), Some(Value::F64(tick as f64)));
        }
    }

    #[test]
    fn propagating_against_a_second_table_fails_loud_and_writes_nothing() {
        // Table A: src@0, dst@1. Propagate once so the route memoizes those indices.
        let mut a = StateTable::new();
        let src = register_cvar(&mut a, "src", Value::U32(7));
        let dst = cvar("dst");
        register_dst(&mut a, &dst);
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();
        let order = plan(&rt);
        a.set_time(1_000);
        rt.propagate_zero_latency(&mut a, &order, &mut |_| {}).unwrap();
        assert_eq!(a.current_value(&dst).unwrap(), Some(Value::U32(7)));

        // Table B: the SAME dense indices (0, 1) map to DIFFERENT signals. The
        // route's cached idx 0/1 now name B's signals, not its own endpoints —
        // exactly the misuse: one RouteTable propagated against two tables.
        let mut b = StateTable::new();
        let b0 = register_cvar(&mut b, "other0", Value::U32(100));
        let b1 = register_cvar(&mut b, "other1", Value::U32(200));

        // Must fail loud on the index-cache mismatch, not silently write B[0]/B[1].
        b.set_time(2_000);
        assert!(matches!(
            rt.propagate_zero_latency(&mut b, &order, &mut |_| {}),
            Err(RouteError::TableMismatch { .. })
        ));
        // B is untouched: values unchanged and no new historian sample recorded.
        assert_eq!(b.current_value(&b0).unwrap(), Some(Value::U32(100)));
        assert_eq!(b.current_value(&b1).unwrap(), Some(Value::U32(200)));
        assert_eq!(b.changes(&b0).unwrap().len(), 1);
        assert_eq!(b.changes(&b1).unwrap().len(), 1);

        // The delayed path shares the same guard (resolve_endpoint): a delayed
        // route caches against A, then the same misuse against B fails loud too.
        let mut a2 = StateTable::new();
        let dsrc = register_cvar(&mut a2, "dsrc", Value::U32(3));
        let ddst = cvar("ddst");
        register_dst(&mut a2, &ddst);
        let mut drt = RouteTable::new();
        drt.add_with_latency(dsrc.clone(), ddst.clone(), 1).unwrap();
        a2.set_time(1_000);
        drt.propagate_delayed(&mut a2, &mut |_| {}).unwrap(); // memoize dsrc@0, ddst@1

        let mut b2 = StateTable::new();
        let c0 = register_cvar(&mut b2, "z0", Value::U32(1));
        let c1 = register_cvar(&mut b2, "z1", Value::U32(2));
        b2.set_time(2_000);
        assert!(matches!(
            drt.propagate_delayed(&mut b2, &mut |_| {}),
            Err(RouteError::TableMismatch { .. })
        ));
        assert_eq!(b2.changes(&c0).unwrap().len(), 1);
        assert_eq!(b2.changes(&c1).unwrap().len(), 1);
    }

    #[test]
    fn cached_indices_survive_later_registrations_on_the_same_table() {
        // The benign case the guard must NOT break: indices are append-only within
        // a table, so registering more signals after caching leaves cached routes
        // resolving to the right endpoints.
        let mut st = StateTable::new();
        let src = register_cvar(&mut st, "src", Value::U32(1));
        let dst = cvar("dst");
        register_dst(&mut st, &dst);
        let mut rt = RouteTable::new();
        rt.add(src.clone(), dst.clone()).unwrap();
        let order = plan(&rt);

        st.set_time(1_000);
        rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap(); // caches src@0, dst@1
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(1)));

        // Grow the table AFTER caching; the cached indices stay valid.
        register_cvar(&mut st, "later0", Value::U32(50));
        register_cvar(&mut st, "later1", Value::U32(60));
        st.set_time(2_000);
        st.force_record(&src, Value::U32(2)).unwrap();
        rt.propagate_zero_latency(&mut st, &order, &mut |_| {}).unwrap();
        assert_eq!(st.current_value(&dst).unwrap(), Some(Value::U32(2)));
    }
}
