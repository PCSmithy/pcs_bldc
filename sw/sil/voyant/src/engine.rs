//! The sim clock + step loop. [`Engine`] owns the [`StateTable`], the
//! [`RouteTable`], and the [`Member`]s, and advances the whole system one tick at a
//! time with [`step`](Engine::step), driving members *only* through the [`Member`]
//! trait. See `architecture.md` §5, `state-route-tables.md` §3.
//!
//! ## The canonical tick order (settled)
//!
//! Discrete sim serializes concurrent physics, so every feedback cycle needs exactly
//! one tick of separation — a **delayed (latency-1) route** is that explicit ZOH cut,
//! while forward (zero-latency) dataflow has no added latency. Each [`step`](Engine::step):
//!
//! 1. **Advance sim time** — `now += tick_period` (monotonic, wall-clock-free);
//!    [`StateTable::set_time`] stamps every record this tick.
//! 2. **Validate the wiring if dirty** (below); a cached invalid verdict re-raises
//!    each step until fixed.
//! 3. **Delayed routes once**, from a snapshot taken *before any member advances*
//!    ([`RouteTable::propagate_delayed`]): each destination gets its source's
//!    end-of-previous-tick value.
//! 4. **Each enabled member, in registration order:** re-resolve the zero-latency
//!    routes in topo order with fresh reads ([`RouteTable::propagate_zero_latency`]) —
//!    a chain `a→b→c` resolves this same tick — then, **if its
//!    [`Cadence`](crate::member::Cadence) says it is due**, `member.advance(dt, st)`
//!    with `dt` = elapsed since its previous advance (a firmware member also flushes
//!    driven cvars, ticks, and samples cvars back out). Propagation reports changed
//!    destinations into per-member input-dirty bits (`OnInputChange`'s due test); an
//!    `OnDemand` member is skipped outright — its bus transfers read the table
//!    directly.
//!
//! Re-running the whole zero-latency DAG per member is semantically identical to
//! per-member resolution (routes are pure copies, `record` dedups) and far simpler;
//! the `M×R` copy cost is a flagged perf seam (`docs/sil/performance.md`). Disabled
//! members are skipped while sim time flows (signals hold their last value).
//!
//! ## Step-time validation (dirty-flag cached)
//!
//! Any wiring mutation sets a **dirty flag**; the next `step` revalidates only when
//! dirty, else reuses the cached verdict + zero-latency topo order. Rewiring anytime
//! is legal — a failure surfaces loudly at the *next* step as an
//! [`EngineError::Route`] (single-driver, acyclicity, forward-flow; see
//! [`RouteTable::validate`]). **Registration order is a design surface**: order
//! members along the signal flow; the validator says when a route needs to be delayed
//! or the members reordered.
//!
//! ## No backend handle (routes are table-mediated)
//!
//! The engine holds **no backend borrow** — propagation is a pure State Table
//! operation. Each firmware instance lives in its own
//! [`FirmwareMember`](crate::FirmwareMember) driving its own backend, so
//! multi-firmware is just multiple `FirmwareMember`s peering in one engine.

use crate::duplex::{tx_rx_ids, DuplexHandle, DuplexPeer, DuplexRouter};
use crate::log::{LogEntry, LogLevel};
use crate::member::{Cadence, Member, MemberCtx};
use crate::route::{RouteError, RouteTable};
use crate::signal::{is_duplex_bus, SignalId, Value};
use crate::state_table::{AccessError, StateTable};
use crate::unit::UnitError;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use thiserror::Error;

/// A registered member plus the engine's own enable flag (engine-gating: the
/// engine skips a disabled member's [`Member::advance`] — simpler than making
/// every member self-gate). Members are **shared** ([`Rc`]/[`RefCell`]): `add_member`
/// hands the caller a typed handle to the same cell the engine steps, so a dual-role
/// member (also a [`DuplexPeer`]) links to a bus directly. `name` is cached at add so
/// validation / lookup never borrows the cell.
struct MemberEntry {
    member: Rc<RefCell<dyn Member>>,
    name: String,
    enabled: bool,
    /// Sim time of this member's last advance — the `dt` baseline for the
    /// non-`EveryStep` cadences. Reset on (re-)enable: a disabled gap is frozen
    /// time, never integrated through.
    last_advance_us: u64,
    /// Next absolute due time for [`Cadence::Periodic`] (drift-free; unused
    /// otherwise).
    next_due_us: u64,
}

/// Errors from engine setup and stepping.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum EngineError {
    #[error(transparent)]
    Route(#[from] RouteError),
    #[error("duplex endpoint {id:?}: {reason}")]
    Duplex { id: String, reason: String },
}

/// The sim clock + step loop. Owns the State Table, Route Table, and members, and
/// holds no backend handle — so a caller can keep its own `Rc<Firmware>` for ad-hoc
/// white-box injection alongside the engine. Members are `'static` (a
/// [`FirmwareMember`](crate::FirmwareMember) owns its firmware via [`Rc`]).
pub struct Engine {
    state: StateTable,
    routes: RouteTable,
    members: Vec<MemberEntry>,
    /// The shared duplex router any initiating member (firmware or model) couples
    /// through; the engine drains + records its transactions each `step`.
    duplex: DuplexRouter,
    tick_period_us: u64,
    now_us: u64,
    /// Wiring changed since the last validation → revalidate at the next `step`.
    dirty: bool,
    /// Cached invalid-wiring verdict (`None` = valid); re-raised each `step` until
    /// the wiring is fixed (a mutation clears it via `dirty`).
    verdict: Option<RouteError>,
    /// Cached enabled zero-latency route order (topological), rebuilt on validate.
    zl_order: Vec<usize>,
    /// Per-member input-dirty bit (indexed like `members`): a route or scenario
    /// write changed a signal in that member's namespace since its last advance.
    /// [`Cadence::OnInputChange`]'s due test; every advance consumes it.
    inputs_dirty: Vec<bool>,
    /// Member name (= signal `<source>` namespace) → `members` index, for the
    /// input-dirty mapping. The table stays dumb — the engine owns this.
    member_idx: HashMap<String, usize>,
}

impl Engine {
    /// A new engine advancing `tick_period_us` of sim time per [`step`](Self::step)
    /// (e.g. 1000 for a 1 kHz cadence). Sim time starts at 0; the first `step`
    /// records at `tick_period_us`.
    pub fn new(tick_period_us: u64) -> Self {
        Self {
            state: StateTable::new(),
            routes: RouteTable::new(),
            members: Vec::new(),
            duplex: DuplexRouter::new(),
            tick_period_us,
            now_us: 0,
            dirty: true,
            verdict: None,
            zl_order: Vec::new(),
            inputs_dirty: Vec::new(),
            member_idx: HashMap::new(),
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

    /// Add a member **by value** and get back a typed shared handle
    /// ([`Rc<RefCell<M>>`](std::rc::Rc)) to the same cell the engine steps — ignorable
    /// for a plain model, linkable for a dual-role member (an `M: DuplexPeer` handle
    /// coerces to `Rc<RefCell<dyn DuplexPeer>>` for [`link_duplex`](Self::link_duplex)).
    /// Members **start enabled**: the engine calls
    /// [`Member::set_enabled(true)`](Member::set_enabled) now (where it registers its
    /// signals) and caches the name. Advance order is registration order — a design
    /// surface for forward flow. Marks the wiring dirty.
    ///
    /// The engine steps each member through `borrow_mut`; holding a `borrow_mut` on the
    /// returned handle across [`step`](Self::step) panics (single-threaded discipline,
    /// loud on misuse — the same rule as a duplex peer's cell).
    pub fn add_member<M: Member + 'static>(&mut self, member: M) -> Rc<RefCell<M>> {
        let rc = Rc::new(RefCell::new(member));
        let name = rc.borrow().name().to_string();
        let cadence = rc.borrow().cadence();
        rc.borrow_mut().set_enabled(true, &mut self.state);
        self.member_idx.insert(name.clone(), self.members.len());
        // Dirty at birth: the first scheduled advance always runs, so an
        // OnInputChange member publishes its resting outputs (a sense chain's
        // bias volts) even in a world where nothing ever drives it.
        self.inputs_dirty.push(true);
        self.members.push(MemberEntry {
            member: rc.clone() as Rc<RefCell<dyn Member>>,
            name,
            enabled: true,
            last_advance_us: self.now_us,
            next_due_us: Self::first_due(cadence, self.now_us),
        });
        self.dirty = true;
        rc
    }

    /// The first [`Cadence::Periodic`] due time from `now` (0 for other cadences —
    /// unused there).
    fn first_due(cadence: Cadence, now_us: u64) -> u64 {
        match cadence {
            Cadence::Periodic { period_us } => now_us + period_us.max(1),
            _ => 0,
        }
    }

    /// Enable or disable a member by name. A disabled member's advance is skipped
    /// (signals hold their last value); re-enabling re-invokes
    /// [`Member::set_enabled(true)`](Member::set_enabled) idempotently. Returns
    /// whether the member was found; marks the wiring dirty when it was.
    pub fn set_member_enabled(&mut self, name: &str, on: bool) -> bool {
        let now = self.now_us;
        if let Some(i) = self.members.iter().position(|e| e.name == name) {
            let entry = &mut self.members[i];
            entry.enabled = on;
            if on {
                // A disabled gap is frozen time: re-baseline rather than hand the
                // next advance a dt spanning the gap. Inputs count as dirty again
                // (the same "first advance always runs" rule as add_member).
                entry.last_advance_us = now;
                entry.next_due_us = Self::first_due(entry.member.borrow().cadence(), now);
                self.inputs_dirty[i] = true;
            }
            entry.member.borrow_mut().set_enabled(on, &mut self.state);
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

    /// Link a [`DuplexPeer`] to a duplex endpoint and return the endpoint's
    /// [`DuplexHandle`]. `endpoint_id` is a SignalId-grammar `spi:<owning-member>:<local>`
    /// (a bus `sig_type`, no modifier). Declares the endpoint (a model endpoint is
    /// declared implicitly here; a firmware endpoint the firmware member re-declares
    /// idempotently), registers its `:tx` / `:rx` event entries, and attaches the peer.
    ///
    /// An initiating **model** keeps the returned handle and drives the bus via
    /// [`MemberCtx::duplex_transfer`](crate::member::MemberCtx::duplex_transfer); a
    /// **firmware** initiator drives it through its C SPI upcall (the handle is then a
    /// convenience the caller may ignore).
    pub fn link_duplex(
        &mut self,
        endpoint_id: &str,
        peer: Rc<RefCell<dyn DuplexPeer>>,
    ) -> Result<DuplexHandle, EngineError> {
        let bad = |reason: &str| EngineError::Duplex {
            id: endpoint_id.to_string(),
            reason: reason.to_string(),
        };
        let sid = SignalId::parse(endpoint_id).map_err(|e| bad(&e.to_string()))?;
        if sid.modifier().is_some() {
            return Err(bad("endpoint id must not carry a :modifier"));
        }
        if !is_duplex_bus(sid.sig_type()) {
            return Err(bad("sig_type is not a duplex bus"));
        }
        let handle = self.duplex.declare(endpoint_id);
        self.duplex.link(endpoint_id, peer);
        // Register the `:tx` / `:rx` event entries (idempotent with a firmware declare).
        let (tx_id, rx_id) =
            tx_rx_ids(sid.sig_type(), sid.source(), sid.name()).map_err(|e| bad(&e.to_string()))?;
        let _ = self.state.register(tx_id, None);
        let _ = self.state.register(rx_id, None);
        Ok(handle)
    }

    /// Advance the whole system one tick (the canonical order — see the module
    /// docs). Each call moves sim time forward by one period. Returns
    /// [`EngineError::Route`] if the wiring is invalid (raised at this step, and
    /// re-raised each step until fixed).
    pub fn step(&mut self) -> Result<(), EngineError> {
        // 1. Sim time advances (monotonic, deterministic — no wall-clock).
        self.now_us += self.tick_period_us;
        self.state.set_time(self.now_us);

        // A duplex link that still names an undeclared endpoint is dangling — warn
        // once (a peer wired to an endpoint no member ever brings up).
        for id in self.duplex.take_dangling() {
            self.state.log(
                LogLevel::Warning,
                "duplex",
                format!("duplex link {id:?} names an endpoint that was never declared"),
            );
        }

        // 2. Validate the wiring if dirty; cache the verdict + zero-latency order.
        if self.dirty {
            let names: Vec<&str> = self.members.iter().map(|e| e.name.as_str()).collect();
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
        //    Changed destinations feed the per-member input-dirty bits.
        {
            let dirt = &mut self.inputs_dirty;
            let map = &self.member_idx;
            self.routes.propagate_delayed_observed(&mut self.state, &mut |dst| {
                if let Some(&j) = map.get(dst.source()) {
                    dirt[j] = true;
                }
            })?;
        }

        // 4. Each enabled member, in registration order: re-resolve the zero-latency
        //    DAG (topo order, fresh reads) — marking input-dirty bits — then advance
        //    if the member's cadence says it is due. An `OnDemand` member is never
        //    scheduled (nor propagated for — later passes keep its inputs fresh for
        //    its bus transfers). `routes`, `state`, and `zl_order` are disjoint from
        //    `members`, so the borrows coexist. Propagation is table-only; the
        //    member syncs its own firmware mirrors.
        let tick = self.tick_period_us;
        let now = self.now_us;
        for i in 0..self.members.len() {
            if !self.members[i].enabled {
                continue;
            }
            let member = Rc::clone(&self.members[i].member);
            let cadence = member.borrow().cadence();
            if cadence == Cadence::OnDemand {
                continue;
            }
            {
                let dirt = &mut self.inputs_dirty;
                let map = &self.member_idx;
                self.routes.propagate_zero_latency_observed(
                    &mut self.state,
                    &self.zl_order,
                    &mut |dst| {
                        if let Some(&j) = map.get(dst.source()) {
                            dirt[j] = true;
                        }
                    },
                )?;
            }
            let entry = &mut self.members[i];
            let due = match cadence {
                Cadence::EveryStep => true,
                Cadence::Periodic { .. } => now >= entry.next_due_us,
                Cadence::OnInputChange => self.inputs_dirty[i],
                Cadence::OnDemand => unreachable!("skipped above"),
            };
            if !due {
                continue;
            }
            // dt = elapsed since the previous advance (exactly the grid step for
            // EveryStep — a disabled gap never leaks in, see set_member_enabled).
            let dt = match cadence {
                Cadence::EveryStep => tick,
                _ => now - entry.last_advance_us,
            };
            entry.last_advance_us = now;
            if let Cadence::Periodic { period_us } = cadence {
                let period = period_us.max(1);
                while entry.next_due_us <= now {
                    entry.next_due_us += period;
                }
            }
            let dirty = std::mem::replace(&mut self.inputs_dirty[i], false);
            let mut ctx = MemberCtx::new(&mut self.state, &self.duplex);
            ctx.inputs_dirty = dirty;
            member.borrow_mut().advance(dt, &mut ctx);
        }

        // 5. Drain the duplex router: force-record every exchange this tick as
        //    `<endpoint>:tx` / `:rx` event entries — identical for firmware- and
        //    model-initiated transfers. (Same-tick transactions share a timestamp;
        //    finer event stamps arrive with the sub-tick grid work.)
        self.record_duplex_transactions();
        Ok(())
    }

    /// Drain the duplex router and force-record each `(endpoint, tx, rx)` exchange
    /// into the endpoint's `:tx` / `:rx` event entries. Events are never deduped
    /// (consecutive identical polls are distinct transactions).
    fn record_duplex_transactions(&mut self) {
        for (endpoint, tx, rx) in self.duplex.drain() {
            let sid = match SignalId::parse(&endpoint) {
                Ok(s) => s,
                Err(e) => {
                    self.state.log(
                        LogLevel::Warning,
                        "duplex",
                        format!("duplex endpoint {endpoint:?} is not a valid id: {e}"),
                    );
                    continue;
                }
            };
            let (tx_id, rx_id) = tx_rx_ids(sid.sig_type(), sid.source(), sid.name())
                .expect("a valid bus id yields valid tx/rx ids");
            if let Err(e) = self.state.force_record(&tx_id, Value::Bytes(tx)) {
                self.state
                    .log(LogLevel::Warning, "duplex", format!("duplex tx record {tx_id} failed: {e}"));
            }
            if let Err(e) = self.state.force_record(&rx_id, Value::Bytes(rx)) {
                self.state
                    .log(LogLevel::Warning, "duplex", format!("duplex rx record {rx_id} failed: {e}"));
            }
        }
    }

    /// Scenario-side duplex transfer: act as the initiator on `handle` (a bench
    /// master exercising a peer between steps). Same synchronous exchange a member
    /// runs via [`MemberCtx::duplex_transfer`]; the transaction is buffered and
    /// force-recorded at the next [`step`](Self::step)'s drain.
    pub fn duplex_transfer(&mut self, handle: DuplexHandle, tx: &[u8]) -> Option<Vec<u8>> {
        self.duplex.transfer(handle, tx, &mut self.state)
    }

    /// Mirror every enabled member's outbound state into the table at the current sim
    /// time ([`Member::mirror`]), advancing nothing. A scenario calls this before
    /// asserting on a signal whose member mirrors on a cadence of its own.
    pub fn mirror_now(&mut self) {
        for entry in &self.members {
            if entry.enabled {
                let mut ctx = MemberCtx::new(&mut self.state, &self.duplex);
                entry.member.borrow_mut().mirror(&mut ctx);
            }
        }
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

    /// Serialize the historian as a versioned binary trace stream (see
    /// [`crate::trace`]). `prefix_filter`: keep only signals whose id starts with one
    /// of the prefixes; `None` = every signal. The data lives in the owned
    /// [`StateTable`], so this delegates there.
    pub fn dump_trace(
        &self,
        w: &mut impl std::io::Write,
        prefix_filter: Option<&[&str]>,
    ) -> std::io::Result<()> {
        crate::trace::write_trace(&self.state, w, prefix_filter)
    }

    // --- string-keyed scenario API (delegates to the owned State Table) ---
    // A test holding an `Engine` writes/reads by id string, without a `Firmware` handle.

    /// String-keyed table write. Delegates to [`StateTable::write`]. A driven `cvar`
    /// reaches firmware memory at the owning member's in-sync flush on the next
    /// [`step`](Self::step) — not immediately. A write into a member's namespace
    /// marks that member input-dirty (a scenario write is an input event).
    pub fn write(&mut self, id: &str, value: impl Into<Value>) -> Result<(), AccessError> {
        self.state.write(id, value)?;
        if let Some(&j) = id.split(':').nth(1).and_then(|src| self.member_idx.get(src)) {
            self.inputs_dirty[j] = true;
        }
        Ok(())
    }

    /// String-keyed current-value read. Delegates to [`StateTable::read`].
    pub fn read(&self, id: &str) -> Result<Option<Value>, AccessError> {
        self.state.read(id)
    }

    /// Extend the unit-conversion registry at runtime. Delegates to
    /// [`StateTable::add_unit`].
    pub fn add_unit(
        &mut self,
        name: &str,
        dimension: &str,
        scale: f64,
        offset: f64,
    ) -> Result<(), UnitError> {
        self.state.add_unit(name, dimension, scale, offset)
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
    use crate::backend::{Backend, CvarEnumeration, FirmwareMember};
    use crate::irq::{IrqKind, IrqOp, IrqRendezvous};
    use crate::member::{vsig_id, RampModel};
    use crate::signal::Value;
    use crate::state_table::TableError;
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;
    use std::rc::Rc;

    /// Stand-in handler address for the kernel tick a firmware's port registers.
    const MOCK_SYSTICK: usize = 0x5157;

    /// A pure-Rust [`Backend`] that records the order of its calls, so a test can
    /// prove *when* in a step a route write vs the firmware's own execution happens.
    /// Its "port" registers a periodic systick the way the real fiber port does, so a
    /// member has something due each step; the tick is all the firmware it runs. Reads
    /// return a stored value if written, else a `U32` of the tick count (a
    /// firmware-less ramp for the sampling tests).
    struct MockBackend {
        log: RefCell<Vec<String>>,
        ticks: Cell<u32>,
        cvars: RefCell<HashMap<String, Value>>,
        leaves: Vec<String>,
        irq: IrqRendezvous,
    }

    impl MockBackend {
        /// A backend whose systick comes due every `period_us`, with no cvar leaves.
        fn new(period_us: u64) -> Self {
            let irq = IrqRendezvous::default();
            irq.register(MOCK_SYSTICK, IrqKind::Periodic, period_us, 15);
            Self {
                log: RefCell::new(Vec::new()),
                ticks: Cell::new(0),
                cvars: RefCell::new(HashMap::new()),
                leaves: Vec::new(),
                irq,
            }
        }

        fn with_leaves(period_us: u64, leaves: &[&str]) -> Self {
            Self {
                leaves: leaves.iter().map(|s| (*s).to_string()).collect(),
                ..Self::new(period_us)
            }
        }
    }

    impl Backend for MockBackend {
        fn advance_time(&self, _elapsed_us: u64) {}
        fn dispatch_isr(&self, _handler: usize) -> bool {
            self.ticks.set(self.ticks.get() + 1);
            self.log.borrow_mut().push("tick".into());
            true
        }
        fn irq_register(&self, handler: usize, kind: IrqKind, rate: u64, priority: u8) -> i32 {
            self.irq.register(handler, kind, rate, priority)
        }
        fn irq_ops_since(&self, from: usize) -> Vec<IrqOp> {
            self.irq.ops_since(from)
        }
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
        fn enumerate_cvars(&self, _threshold: usize, _includes: &[String]) -> CvarEnumeration {
            CvarEnumeration {
                leaves: self.leaves.iter().map(|p| (p.clone(), None)).collect(),
                ..CvarEnumeration::default()
            }
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
        fn advance(&mut self, _dt_us: u64, _ctx: &mut MemberCtx) {
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
        fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
            let input = match ctx.st.current_value(&self.in_id()).ok().flatten() {
                Some(Value::U32(x)) => x,
                _ => 0,
            };
            self.out = input.wrapping_add(self.step);
            let _ = ctx.st.record(&self.out_id(), Value::U32(self.out));
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
    fn step_flushes_driven_dest_before_the_firmware_runs() {
        // A model member's output routed to a firmware member's DRIVEN cvar must
        // reach firmware memory BEFORE any of that firmware's code runs in the same
        // step. Member order [model, firmware] makes it hold — the model records its
        // vsig, the firmware member's pre-advance zero-latency pass records it into
        // the cvar entry, and the firmware member flushes it at in-sync.
        let be = Rc::new(MockBackend::with_leaves(1_000, &["sensor_in"]));
        let mut eng = Engine::new(1_000);
        eng.add_member(RampModel::new("ramp", 1000.0, None));
        let fw = FirmwareMember::with_backend("fw", be.clone());
        eng.add_member(fw);
        // The auto-mirrored cvar is registered under the member's own name ("fw").
        let sensor_in = SignalId::new("cvar", "fw", "sensor_in", None).unwrap();
        eng.add_route(vsig_id("ramp", "value").unwrap(), sensor_in)
            .unwrap();

        eng.step().unwrap();

        let log = be.log.borrow();
        let w = log.iter().position(|e| e == "write:sensor_in").unwrap();
        let t = log.iter().position(|e| e == "tick").unwrap();
        assert!(w < t, "driven flush must precede the kernel tick, got {log:?}");
        assert_eq!(be.cvars.borrow().get("sensor_in"), Some(&Value::F64(1.0)));
    }

    #[test]
    fn time_advances_by_tick_period() {
        let be = Rc::new(MockBackend::with_leaves(1_000, &["counter"]));
        let mut eng = Engine::new(1_000);
        let id = SignalId::new("cvar", "fw", "counter", None).unwrap();
        eng.add_member(FirmwareMember::with_backend("fw", be.clone()));

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
            eng.add_member(OrderModel {
                name: name.to_string(),
                log: Rc::clone(&log),
            });
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
        let be = Rc::new(MockBackend::with_leaves(1_000, &["ramp_counter"]));
        let mut eng = Engine::new(1_000);
        let id = SignalId::new("cvar", "fw", "ramp_counter", None).unwrap();
        eng.add_member(FirmwareMember::with_backend("fw", be.clone()));

        for _ in 0..3 {
            eng.step().unwrap();
        }
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(Value::U32(3)));
        assert_eq!(eng.state().changes(&id).unwrap().len(), 3);
    }

    #[test]
    fn records_model_vsig_each_tick() {
        let mut eng = Engine::new(1_000);
        eng.add_member(RampModel::new("ramp", 1000.0, Some("counts")));
        let id = vsig_id("ramp", "value").unwrap();

        assert_eq!(eng.state().current_value(&id).unwrap(), None);
        for _ in 0..5 {
            eng.step().unwrap();
        }
        assert_eq!(eng.state().changes(&id).unwrap().len(), 5);
        assert_eq!(eng.state().current_value(&id).unwrap(), Some(Value::F64(5.0)));
        assert_eq!(eng.state().value_at(&id, 2_500).unwrap(), Some(Value::F64(2.0)));
    }

    #[test]
    fn empty_step_advances_firmware_and_time() {
        let be = Rc::new(MockBackend::new(2_000));
        let mut eng = Engine::new(2_000);
        eng.add_member(FirmwareMember::with_backend("fw", be.clone()));
        eng.step().unwrap();
        eng.step().unwrap();
        assert_eq!(eng.now_us(), 4_000);
        assert_eq!(be.ticks.get(), 2);
        assert_eq!(*be.log.borrow(), vec!["tick", "tick"]);
    }

    #[test]
    fn disabled_member_is_skipped_then_resumes() {
        let be = Rc::new(MockBackend::new(1_000));
        let mut eng = Engine::new(1_000);
        eng.add_member(FirmwareMember::with_backend("fw", be.clone()));

        assert!(eng.set_member_enabled("fw", false));
        eng.step().unwrap(); // skipped: the member never advances
        assert_eq!(be.ticks.get(), 0);
        assert_eq!(eng.now_us(), 1_000); // but sim time still flows

        assert!(eng.set_member_enabled("fw", true));
        eng.step().unwrap();
        assert_eq!(be.ticks.get(), 1);

        assert!(!eng.set_member_enabled("nope", false));
    }

    #[test]
    fn route_source_unregistered_errors() {
        let be = Rc::new(MockBackend::new(1_000));
        let mut eng = Engine::new(1_000);
        eng.add_member(FirmwareMember::with_backend("fw", be.clone()));
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
        eng.add_member(OrderModel {
            name: "nop".into(),
            log: Rc::clone(&log),
        });
        eng.state_mut_seed("cvar:test:a", Value::U32(9));
        eng.state_mut_seed("cvar:test:b", Value::U32(0));
        eng.state_mut_seed("cvar:test:c", Value::U32(0));
        eng.add_route(cvar("a"), cvar("b")).unwrap();
        eng.add_route(cvar("b"), cvar("c")).unwrap();

        eng.step().unwrap();
        assert_eq!(eng.state().current_value(&cvar("b")).unwrap(), Some(Value::U32(9)));
        assert_eq!(eng.state().current_value(&cvar("c")).unwrap(), Some(Value::U32(9)));
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
        eng.add_member(AdderModel::new("a", 1)); // out = in + 1
        eng.add_member(AdderModel::new("b", 10)); // out = in + 10

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
                Some(Value::U32(x)) => x,
                _ => 0,
            };
            let bo = match eng.state().current_value(&b_out).unwrap() {
                Some(Value::U32(x)) => x,
                _ => 0,
            };
            assert_eq!((ao, bo), (*ea, *eb), "tick {}", i + 1);
        }
    }

    #[test]
    fn multi_driver_errors_at_step_unless_suspended() {
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(1_000);
        eng.add_member(OrderModel {
            name: "nop".into(),
            log,
        }); // drives the per-member zero-latency pass
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
        assert_eq!(eng.state().current_value(&cvar("dst")).unwrap(), Some(Value::U32(1)));
    }

    #[test]
    fn dirty_flag_caches_verdict_and_revalidates_only_on_change() {
        // Invalid wiring errors at step; the same error is re-raised until fixed;
        // removing the offending route lets the next step pass — no rebuild.
        let log = Rc::new(RefCell::new(Vec::new()));
        let mut eng = Engine::new(1_000);
        eng.add_member(OrderModel {
            name: "nop".into(),
            log,
        }); // drives the per-member zero-latency pass
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
        assert_eq!(eng.state().current_value(&cvar("b")).unwrap(), Some(Value::U32(1)));
    }

    // --- model <-> model duplex (no firmware) ----------------------------

    use crate::duplex::{DuplexHandle, DuplexPeer};

    /// One struct, both roles: answers each transfer with its current byte (as a
    /// [`DuplexPeer`]) and advances that byte each tick (as a [`Member`]) — its state
    /// is its own.
    struct SpiResponder {
        name: String,
        next: u8,
    }
    impl DuplexPeer for SpiResponder {
        fn transfer(&mut self, _tx: &[u8], _ctx: &mut MemberCtx) -> Vec<u8> {
            vec![self.next, 0x00]
        }
    }
    impl Member for SpiResponder {
        fn name(&self) -> &str {
            &self.name
        }
        fn advance(&mut self, _dt_us: u64, _ctx: &mut MemberCtx) {
            self.next = self.next.wrapping_add(1);
        }
        fn set_enabled(&mut self, _on: bool, _st: &mut StateTable) {}
    }

    /// A duplex initiator member: initiates a transfer each advance and records the
    /// first response byte as a `vsig`.
    struct SpiInitiator {
        name: String,
        handle: DuplexHandle,
    }
    impl Member for SpiInitiator {
        fn name(&self) -> &str {
            &self.name
        }
        fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
            if let Some(rx) = ctx.duplex_transfer(self.handle, &[0xFF, 0xFF]) {
                let id = vsig_id(&self.name, "rx0").unwrap();
                let _ = ctx.st.record(&id, Value::U32(u32::from(rx[0])));
            }
        }
        fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
            if on {
                let _ = st.register(vsig_id(&self.name, "rx0").unwrap(), None);
            }
        }
    }

    #[test]
    fn model_to_model_duplex_transfers_and_records() {
        // Two models couple over spi:initiator:cs with NO firmware: the initiator transfers
        // mid-advance, the responder answers from its own state synchronously, and the
        // engine force-records the exchange under the model-owned endpoint.
        let mut eng = Engine::new(1_000);
        // The responder is a shared member: added by value, linked to the bus by its
        // handle. It advances (idx 0) before the initiator (idx 1) reads each tick, so
        // its byte starts at 0x0F and the initiator sees 0x10, 0x11, 0x12.
        let responder = eng.add_member(SpiResponder { name: "responder".into(), next: 0x0F });
        let handle = eng.link_duplex("spi:initiator:cs", responder).unwrap();
        eng.add_member(SpiInitiator { name: "initiator".into(), handle });

        for _ in 0..3 {
            eng.step().unwrap();
        }

        // The initiator reads the responder's just-advanced byte: 0x10, 0x11, 0x12.
        assert_eq!(eng.read("vsig:initiator:rx0").unwrap(), Some(Value::U32(0x12)));
        let tx = SignalId::parse("spi:initiator:cs:tx").unwrap();
        let rx = SignalId::parse("spi:initiator:cs:rx").unwrap();
        // Events are force-recorded (never deduped): one transfer/tick = three each.
        assert_eq!(eng.state().changes(&tx).unwrap().len(), 3);
        assert_eq!(eng.state().changes(&rx).unwrap().len(), 3);
        assert_eq!(
            eng.state().current_value(&rx).unwrap(),
            Some(Value::Bytes(vec![0x12, 0x00]))
        );
    }

    #[test]
    fn duplex_transfer_on_unlinked_endpoint_is_none() {
        // A declared-but-unlinked endpoint: the initiator's transfer returns None (a
        // floating bus), nothing is recorded, and the initiator produces no reading.
        let mut eng = Engine::new(1_000);
        let handle = eng.duplex.declare("spi:initiator:cs"); // no peer linked
        eng.add_member(SpiInitiator { name: "initiator".into(), handle });
        eng.step().unwrap();
        assert_eq!(eng.read("vsig:initiator:rx0").unwrap(), None);
        assert!(eng.state().signals().all(|s| s.as_str() != "spi:initiator:cs:rx"));
    }

    #[test]
    fn dangling_duplex_link_warns_once() {
        // A peer linked to an endpoint no member ever declares is dangling: the engine
        // warns once at step time, not per tick.
        let mut eng = Engine::new(1_000);
        eng.duplex
            .link("spi:ghost:cs", Rc::new(RefCell::new(SpiResponder { name: "ghost".into(), next: 0 })));
        eng.step().unwrap();
        assert_eq!(
            eng.take_logs()
                .iter()
                .filter(|e| e.message.contains("spi:ghost:cs"))
                .count(),
            1
        );
        eng.step().unwrap();
        assert!(eng
            .take_logs()
            .iter()
            .all(|e| !e.message.contains("spi:ghost:cs")));
    }

    #[test]
    fn engine_delegates_add_unit_and_unit_ask_write_read() {
        // A model registers vsig:ramp:value with canonical unit "rad"; the engine's
        // add_unit / write / read delegates carry the whole unit boundary through.
        let mut eng = Engine::new(1_000);
        eng.add_member(RampModel::new("ramp", 0.0, Some("rad")));
        // A runtime-added unit is usable immediately (deg is a built-in; add a scaled
        // one to prove the delegate reaches the registry).
        eng.add_unit("turn", "angle", std::f64::consts::TAU, 0.0).unwrap();
        eng.write("vsig:ramp:value[deg]", 90.0).unwrap();
        match eng.read("vsig:ramp:value[turn]").unwrap() {
            Some(Value::F64(x)) => assert!((x - 0.25).abs() < 1e-9), // 90 deg == 0.25 turn
            other => panic!("expected converted F64, got {other:?}"),
        }
    }

    // --- member cadence ---------------------------------------------------

    use crate::member::Cadence;

    /// A member that records each advance's `(dt, ctx.inputs_dirty)` and registers
    /// one `in` signal — the probe for the cadence scheduling tests.
    struct CadencedProbe {
        name: String,
        cadence: Cadence,
        log: Rc<RefCell<Vec<(u64, bool)>>>,
    }

    impl CadencedProbe {
        fn new(name: &str, cadence: Cadence) -> (Self, Rc<RefCell<Vec<(u64, bool)>>>) {
            let log = Rc::new(RefCell::new(Vec::new()));
            (
                Self {
                    name: name.to_string(),
                    cadence,
                    log: log.clone(),
                },
                log,
            )
        }
    }

    impl Member for CadencedProbe {
        fn name(&self) -> &str {
            &self.name
        }
        fn cadence(&self) -> Cadence {
            self.cadence
        }
        fn advance(&mut self, dt_us: u64, ctx: &mut MemberCtx) {
            self.log.borrow_mut().push((dt_us, ctx.inputs_dirty()));
        }
        fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
            if on {
                let _ = st.register(vsig_id(&self.name, "in").unwrap(), None);
            }
        }
    }

    #[test]
    fn periodic_cadence_fires_on_absolute_due_times_with_elapsed_dt() {
        // Grid 1000, period 2500: due times are 2500/5000/7500/10000 — the member
        // fires at the first step at-or-after each (3000, 5000, 8000, 10000) with
        // dt = elapsed since its previous advance. Absolute due times never drift.
        let mut eng = Engine::new(1_000);
        let (probe, log) = CadencedProbe::new("probe", Cadence::Periodic { period_us: 2_500 });
        eng.add_member(probe);
        for _ in 0..10 {
            eng.step().unwrap();
        }
        let dts: Vec<u64> = log.borrow().iter().map(|(dt, _)| *dt).collect();
        assert_eq!(dts, vec![3_000, 2_000, 3_000, 2_000]);
    }

    #[test]
    fn on_input_change_cadence_follows_routed_changes_and_scenario_writes() {
        // ramp (EveryStep) drives probe:in zero-latency: the probe advances every
        // step the delivery changes its input, stops when the route is suspended,
        // and a scenario write is an input event.
        let mut eng = Engine::new(1_000);
        eng.add_member(RampModel::new("ramp", 1000.0, None)); // changes every step
        let (probe, log) = CadencedProbe::new("probe", Cadence::OnInputChange);
        eng.add_member(probe);
        let src = vsig_id("ramp", "value").unwrap();
        let dst = vsig_id("probe", "in").unwrap();
        eng.add_route(src.clone(), dst.clone()).unwrap();

        for _ in 0..3 {
            eng.step().unwrap();
        }
        assert_eq!(log.borrow().len(), 3, "a changing routed input is due every step");
        assert!(log.borrow().iter().all(|(_, dirty)| *dirty), "advance sees its dirty flag");

        eng.suspend_route(&src, &dst).unwrap();
        for _ in 0..3 {
            eng.step().unwrap();
        }
        assert_eq!(log.borrow().len(), 3, "no delivery, no advance");

        eng.write("vsig:probe:in", 42.0).unwrap();
        eng.step().unwrap();
        assert_eq!(log.borrow().len(), 4, "a scenario write is an input event");
        // dt spans the quiet interval: last advance at 3000, write lands at 7000.
        assert_eq!(log.borrow().last().copied(), Some((4_000, true)));
    }

    #[test]
    fn on_demand_member_is_never_scheduled() {
        let mut eng = Engine::new(1_000);
        let (probe, log) = CadencedProbe::new("probe", Cadence::OnDemand);
        eng.add_member(probe);
        eng.write("vsig:probe:in", 1.0).unwrap(); // even a dirty input schedules nothing
        for _ in 0..5 {
            eng.step().unwrap();
        }
        assert!(log.borrow().is_empty());
    }

    #[test]
    fn every_step_dt_is_the_grid_step_even_across_a_disabled_gap() {
        // The zero-migration guarantee: an EveryStep member's dt is exactly the
        // grid step — a disabled gap is frozen time, never handed to advance.
        let mut eng = Engine::new(1_000);
        let (probe, log) = CadencedProbe::new("probe", Cadence::EveryStep);
        eng.add_member(probe);
        eng.step().unwrap();
        eng.set_member_enabled("probe", false);
        for _ in 0..3 {
            eng.step().unwrap();
        }
        eng.set_member_enabled("probe", true);
        eng.step().unwrap();
        let dts: Vec<u64> = log.borrow().iter().map(|(dt, _)| *dt).collect();
        assert_eq!(dts, vec![1_000, 1_000]);
    }

    // --- test-only helpers on the engine ---------------------------------

    impl Engine {
        /// Register + seed a signal directly on the engine's table (test scaffolding
        /// for wiring routes without a driving member).
        fn state_mut_seed(&mut self, id: &str, v: Value) {
            let id = SignalId::parse(id).unwrap();
            let _ = self.state.register(id.clone(), None);
            self.state.force_record(&id, v).unwrap();
        }
    }
}
