//! The State Table: signal registry + per-signal change-logged history +
//! current-value cache + injection overrides + bounded retention.
//!
//! This is *pure data* — no FFI, no DWARF. It is fed by [`StateTable::record`]
//! (a sampler reads a backing's live value and records it here) and queried by
//! [`StateTable::current_value`] (O(1)) / [`StateTable::value_at`] (O(log n),
//! zero-order-hold). Each signal owns its own change-log ([D12]: the State
//! Table *is* the historian).
//!
//! Change-logging: a `record` is stored only when the value differs from the
//! current cached value by more than the signal's epsilon (default 1e-3 for
//! floats; exact for everything else). Overridden signals ignore `record` (the
//! injection pin). Retention evicts old samples per a time window (fast mode
//! sets it to unbounded; realtime keeps a window).

use crate::log::{LogEntry, LogLevel, LogRing};
use crate::signal::{SignalId, Value};
use indexmap::IndexSet;
use std::collections::{HashMap, HashSet, VecDeque};
use std::time::Duration;
use thiserror::Error;

/// Default float change-detection epsilon ("moved, not noise" — D12).
const DEFAULT_EPSILON: f64 = 1e-3;
/// Default realtime retention window; fast mode overrides to `None` (unbounded).
const DEFAULT_RETENTION: Duration = Duration::from_secs(30);
/// Default log-ring capacity (drop-oldest beyond this; see [`LogRing`]).
const DEFAULT_LOG_CAPACITY: usize = 4096;

#[derive(Debug, Clone)]
pub struct StateTableConfig {
    /// Global retention window; `None` = unbounded (fast mode, full capture).
    pub retention: Option<Duration>,
    /// Per-signal retention overrides.
    pub signal_retention: HashMap<SignalId, Duration>,
    /// Global float change-detection epsilon.
    pub epsilon: f64,
    /// Per-signal epsilon overrides.
    pub signal_epsilon: HashMap<SignalId, f64>,
    /// Capacity of the sim-time-stamped log ring (drop-oldest beyond this).
    pub log_capacity: usize,
}

impl Default for StateTableConfig {
    fn default() -> Self {
        Self {
            retention: Some(DEFAULT_RETENTION),
            signal_retention: HashMap::new(),
            epsilon: DEFAULT_EPSILON,
            signal_epsilon: HashMap::new(),
            log_capacity: DEFAULT_LOG_CAPACITY,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum TableError {
    #[error("unknown signal: {0}")]
    UnknownSignal(SignalId),
    #[error("conflicting re-registration of {signal}: existing unit {existing:?} != requested {requested:?}")]
    ConflictingUnit {
        signal: SignalId,
        existing: Option<String>,
        requested: Option<String>,
    },
    #[error("tick {requested_tick} for {signal} is before the retained window (oldest {oldest_available})")]
    OutOfWindow {
        signal: SignalId,
        requested_tick: u64,
        oldest_available: u64,
    },
}

pub struct StateTable {
    /// Registered signals, in insertion order. The [`IndexSet`] yields a **stable
    /// dense index** per signal (append-only — a signal is never removed, so its
    /// index never changes), which is the key for all hot per-signal storage below
    /// (Tier-2 dense-index fast lane): the string-keyed public API resolves an id to
    /// its index once ([`resolve_index`](Self::resolve_index)) and delegates to the
    /// `*_at` index-keyed internals, keeping per-record string hashing off hot paths.
    signals: IndexSet<SignalId>,
    /// Optional per-signal unit string (metadata; cold — set only at register).
    units: HashMap<SignalId, String>,
    /// Per-signal change-log, indexed by signal index: `(time_us, value)`,
    /// ascending, front-evicted.
    changes: Vec<VecDeque<(u64, Value)>>,
    /// Current value cache, indexed by signal index (O(1) latest; `None` = never
    /// recorded).
    current: Vec<Option<Value>>,
    /// Resolved change-detection epsilon, indexed by signal index (the per-signal
    /// override from config, else the global epsilon — resolved once at register so
    /// the hot `record` path never hashes the config map).
    epsilon: Vec<f64>,
    /// Signals whose `record` is ignored (injection pin), by signal index. A sparse
    /// set: membership check is a cheap `usize` hash, and iteration (for
    /// [`pinned`](Self::pinned)) walks only the pinned few, not all signals.
    overrides: HashSet<usize>,
    /// **Command-write dirty set**, by signal index. Indices a framework command
    /// wrote this cycle (`record` / `force_record` / pin via `set_override`) — as
    /// opposed to a mirror sweep (`record_mirror`), which never marks dirty. A
    /// firmware member drains its own namespace's dirt each tick to know which
    /// `cvar`s to flush back into firmware memory. Deduped by index, so it is
    /// bounded by the distinct-signal count, not tick count.
    dirty: HashSet<usize>,
    /// Signal indices that have had samples evicted (so `value_at` can report
    /// OutOfWindow).
    evicted: HashSet<usize>,
    /// Current sim time (microseconds); the timestamp for `record` and `log`.
    current_time_us: u64,
    /// Sim-time-stamped, bounded log ring (drop-oldest; see [`LogRing`]).
    logs: LogRing,
    config: StateTableConfig,
}

impl Default for StateTable {
    fn default() -> Self {
        Self::new()
    }
}

impl StateTable {
    pub fn new() -> Self {
        Self::with_config(StateTableConfig::default())
    }

    pub fn with_config(config: StateTableConfig) -> Self {
        Self {
            signals: IndexSet::new(),
            units: HashMap::new(),
            changes: Vec::new(),
            current: Vec::new(),
            epsilon: Vec::new(),
            overrides: HashSet::new(),
            dirty: HashSet::new(),
            evicted: HashSet::new(),
            current_time_us: 0,
            logs: LogRing::new(config.log_capacity),
            config,
        }
    }

    /// Set the current sim time (used to timestamp subsequent records).
    pub fn set_time(&mut self, time_us: u64) {
        self.current_time_us = time_us;
    }

    /// Register a signal (optionally with a unit).
    ///
    /// **Idempotent**: re-registering an existing id with *identical* unit metadata
    /// is a benign no-op — a member (e.g. a firmware instance across a reboot)
    /// legitimately re-registers its signals, and a signal's history spans member
    /// lifetimes, so the entry (and its change-log) must be preserved. Re-registering
    /// with a *conflicting* unit is a wiring bug and errors ([`TableError::ConflictingUnit`]).
    pub fn register(&mut self, id: SignalId, unit: Option<&str>) -> Result<(), TableError> {
        if self.signals.contains(&id) {
            let existing = self.units.get(&id).map(String::as_str);
            if existing == unit {
                return Ok(());
            }
            return Err(TableError::ConflictingUnit {
                signal: id.clone(),
                existing: existing.map(str::to_string),
                requested: unit.map(str::to_string),
            });
        }
        if let Some(u) = unit {
            self.units.insert(id.clone(), u.to_string());
        }
        // Resolve the change-detection epsilon once, so the hot `record` path never
        // hashes the per-signal config map.
        let eps = self
            .config
            .signal_epsilon
            .get(&id)
            .copied()
            .unwrap_or(self.config.epsilon);
        // Push the dense slots BEFORE inserting into the index set, so the new
        // signal's index (== the pre-push length) lines up with its slot.
        self.changes.push(VecDeque::new());
        self.current.push(None);
        self.epsilon.push(eps);
        self.signals.insert(id);
        Ok(())
    }

    /// Resolve a signal id to its stable dense index, or `None` if unregistered.
    /// The crate-internal key for the index-keyed hot-path entry points
    /// ([`record_mirror_at`](Self::record_mirror_at) / [`record_at`](Self::record_at)
    /// / [`current_value_at`](Self::current_value_at)); consumers resolve **once**
    /// (at registration / route validation) and then avoid per-tick string hashing.
    pub(crate) fn resolve_index(&self, id: &SignalId) -> Option<usize> {
        self.signals.get_index_of(id)
    }

    /// The signal registered at dense index `idx`, or `None` if `idx` is out of
    /// range. The inverse of [`resolve_index`](Self::resolve_index): it lets a
    /// consumer that memoized an index (route propagation's endpoint cache) verify
    /// that index still names the *expected* signal in **this** table — the guard
    /// against one `RouteTable` being propagated against a different `StateTable`.
    pub(crate) fn id_at(&self, idx: usize) -> Option<&SignalId> {
        self.signals.get_index(idx)
    }

    /// Record a **command-written** value at the current time (route, test, or
    /// model output). Marks the signal dirty so its owning member re-flushes it.
    /// Skips the historian append if overridden or unchanged (within epsilon), but
    /// the dirty mark stands whenever the command actually applied (an unchanged
    /// re-command still means "the framework is driving this," so the value must be
    /// re-asserted into firmware memory each tick). Errors if not registered.
    pub fn record(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        self.record_inner(idx, value, true);
        Ok(())
    }

    /// Record a **mirror** value (a firmware member's end-of-tick sweep of its
    /// memory into the table). Identical dedup/historian/override behavior to
    /// [`record`](Self::record) — a pinned entry ignores it exactly as it ignores a
    /// command `record`, so a sweep can never un-pin the view — but it does **not**
    /// mark the signal dirty (the table is only tracking what memory already holds,
    /// not commanding a write back into it).
    pub fn record_mirror(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        self.record_inner(idx, value, false);
        Ok(())
    }

    /// **Index-keyed mirror record** — the mirror-sweep hot path. Identical
    /// semantics to [`record_mirror`](Self::record_mirror) (dedup / override / no
    /// dirty mark) but keyed by a pre-resolved dense index, so the whole-namespace
    /// sweep never hashes a `SignalId` string. `idx` must come from
    /// [`resolve_index`](Self::resolve_index) (i.e. a registered signal).
    pub(crate) fn record_mirror_at(&mut self, idx: usize, value: Value) {
        self.record_inner(idx, value, false);
    }

    /// **Index-keyed command record** — the route-propagation hot path. Identical
    /// semantics to [`record`](Self::record) (dedup / override / marks dirty) but
    /// keyed by a pre-resolved dense index. `idx` must come from
    /// [`resolve_index`](Self::resolve_index).
    pub(crate) fn record_at(&mut self, idx: usize, value: Value) {
        self.record_inner(idx, value, true);
    }

    fn record_inner(&mut self, idx: usize, value: Value, command: bool) {
        if self.overrides.contains(&idx) {
            // Pinned: both command and mirror records are ignored (the pin holds
            // the current view). A pinned entry is re-asserted via the pinned-flush
            // union, not via record.
            return;
        }
        if command {
            self.dirty.insert(idx);
        }
        if let Some(cur) = &self.current[idx] {
            if cur.approx_eq(&value, self.epsilon[idx]) {
                return;
            }
        }
        self.append(idx, value);
    }

    /// Record unconditionally (bypasses change-detection and overrides). Counts as
    /// a command write (marks the signal dirty).
    pub fn force_record(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        self.dirty.insert(idx);
        self.append(idx, value);
        Ok(())
    }

    /// Current (last-recorded) value, O(1). `None` if never recorded.
    pub fn current_value(&self, id: &SignalId) -> Result<Option<&Value>, TableError> {
        let idx = self.ensure(id)?;
        Ok(self.current[idx].as_ref())
    }

    /// Index-keyed current value (route propagation / port-cache fill hot paths).
    /// `idx` must come from [`resolve_index`](Self::resolve_index).
    pub(crate) fn current_value_at(&self, idx: usize) -> Option<&Value> {
        self.current[idx].as_ref()
    }

    /// Value in effect at `time_us` (zero-order hold: the last sample at or
    /// before it), O(log n). `None` if before the first sample and nothing was
    /// evicted; `OutOfWindow` if it fell out of the retained window.
    pub fn value_at(&self, id: &SignalId, time_us: u64) -> Result<Option<&Value>, TableError> {
        let si = self.ensure(id)?;
        let dq = &self.changes[si];
        if dq.is_empty() {
            return Ok(None);
        }
        let idx = dq.partition_point(|&(t, _)| t <= time_us);
        if idx == 0 {
            // Requested time precedes all retained samples.
            if self.evicted.contains(&si) {
                return Err(TableError::OutOfWindow {
                    signal: id.clone(),
                    requested_tick: time_us,
                    oldest_available: dq[0].0,
                });
            }
            return Ok(None);
        }
        Ok(Some(&dq[idx - 1].1))
    }

    /// Pin/unpin a signal (overridden signals ignore `record`). Pinning marks the
    /// signal dirty: a pin is a continuous drive, so the pinned value must be
    /// asserted into firmware memory from the next tick onward (and every tick
    /// after — see [`pinned`](Self::pinned)). Unpinning does not mark dirty; the
    /// signal simply resumes being mirrored.
    pub fn set_override(&mut self, id: &SignalId, on: bool) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        if on {
            self.overrides.insert(idx);
            self.dirty.insert(idx);
        } else {
            self.overrides.remove(&idx);
        }
        Ok(())
    }

    /// Remove and return the dirty ids whose `<source>` segment equals `source`
    /// (a member drains its **own** namespace; other members' dirt is left in
    /// place for their own drain). Deterministic order (sorted by id string).
    pub fn take_dirty(&mut self, source: &str) -> Vec<SignalId> {
        // Collect the dirty indices whose signal belongs to `source`, remove them,
        // then materialize the ids (deterministic, sorted). The dirty set is small
        // (distinct signals command-written this cycle), so this stays cheap.
        let mine_idx: Vec<usize> = self
            .dirty
            .iter()
            .copied()
            .filter(|&idx| self.signals.get_index(idx).is_some_and(|id| id.source() == source))
            .collect();
        for idx in &mine_idx {
            self.dirty.remove(idx);
        }
        let mut mine: Vec<SignalId> = mine_idx
            .iter()
            .map(|&idx| self.signals.get_index(idx).unwrap().clone())
            .collect();
        mine.sort_by(|a, b| a.as_str().cmp(b.as_str()));
        mine
    }

    /// The currently-pinned (overridden) ids whose `<source>` segment equals
    /// `source`, for the per-tick pinned-flush union (a pin re-asserts every tick,
    /// even when not freshly dirty). Deterministic order.
    pub fn pinned(&self, source: &str) -> Vec<SignalId> {
        let mut ids: Vec<SignalId> = self
            .overrides
            .iter()
            .filter_map(|&idx| self.signals.get_index(idx))
            .filter(|id| id.source() == source)
            .cloned()
            .collect();
        ids.sort_by(|a, b| a.as_str().cmp(b.as_str()));
        ids
    }

    /// The change-log for a signal (timestamped samples), for dump/inspection.
    pub fn changes(&self, id: &SignalId) -> Option<&VecDeque<(u64, Value)>> {
        self.signals.get_index_of(id).map(|idx| &self.changes[idx])
    }

    /// Emit a log entry, stamped with the table's **current sim time**. The
    /// caller supplies only the severity, a `source` tag (a member name, or a
    /// driver tag), and a message — never the timestamp, so members cannot fake
    /// sim time and logging can never perturb behaviour (determinism, D7). The
    /// ring drops the oldest entry when full (see [`take_logs`](Self::take_logs)
    /// / [`dropped_logs`](Self::dropped_logs)).
    pub fn log(&mut self, level: LogLevel, source: &str, message: impl Into<String>) {
        self.logs.push(LogEntry {
            time_us: self.current_time_us,
            level,
            source: source.to_string(),
            message: message.into(),
        });
    }

    /// Drain every buffered log entry (oldest first), leaving the ring empty.
    /// The [`dropped_logs`](Self::dropped_logs) count survives the drain.
    pub fn take_logs(&mut self) -> Vec<LogEntry> {
        self.logs.take()
    }

    /// Peek the buffered log entries without draining.
    pub fn logs(&self) -> &VecDeque<LogEntry> {
        self.logs.peek()
    }

    /// How many log entries were dropped (evicted before a drain) over this
    /// table's life — nonzero means a log storm outran [`take_logs`](Self::take_logs).
    pub fn dropped_logs(&self) -> u64 {
        self.logs.dropped()
    }

    pub fn signals(&self) -> impl Iterator<Item = &SignalId> {
        self.signals.iter()
    }
    pub fn len(&self) -> usize {
        self.signals.len()
    }
    pub fn is_empty(&self) -> bool {
        self.signals.is_empty()
    }

    // --- internals -------------------------------------------------------

    /// Resolve a signal to its dense index, or [`TableError::UnknownSignal`].
    fn ensure(&self, id: &SignalId) -> Result<usize, TableError> {
        self.signals
            .get_index_of(id)
            .ok_or_else(|| TableError::UnknownSignal(id.clone()))
    }

    fn append(&mut self, idx: usize, value: Value) {
        let t = self.current_time_us;
        self.current[idx] = Some(value.clone());
        self.changes[idx].push_back((t, value));
        self.evict(idx);
    }

    /// Drop samples older than the retention window, keeping the one sample that
    /// is still "active" at the cutoff (ZOH needs it).
    fn evict(&mut self, idx: usize) {
        // Per-signal retention override (if any) is keyed by id — resolve the
        // window first, then mutate the dense slots (disjoint borrows).
        let window = {
            let id = self.signals.get_index(idx).unwrap();
            self.config
                .signal_retention
                .get(id)
                .copied()
                .or(self.config.retention)
        };
        let Some(window) = window else { return };
        let cutoff = self
            .current_time_us
            .saturating_sub(window.as_micros() as u64);
        let dq = &mut self.changes[idx];
        let mut dropped = false;
        while dq.len() > 1 && dq[1].0 <= cutoff {
            dq.pop_front();
            dropped = true;
        }
        if dropped {
            self.evicted.insert(idx);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::signal::Value;

    fn id(s: &str) -> SignalId {
        SignalId::parse(s).unwrap()
    }

    #[test]
    fn change_logs_and_dedups() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), Some("counts")).unwrap();

        for (t, v) in [(1_000, 5u32), (2_000, 5), (3_000, 5), (4_000, 9)] {
            st.set_time(t);
            st.record(&a, Value::U32(v)).unwrap();
        }
        // 5,5,5,9 -> two samples (5 @1000, 9 @4000)
        let log = st.changes(&a).unwrap();
        assert_eq!(log.len(), 2);
        assert_eq!(log[0], (1_000, Value::U32(5)));
        assert_eq!(log[1], (4_000, Value::U32(9)));
        assert_eq!(st.current_value(&a).unwrap(), Some(&Value::U32(9)));
    }

    #[test]
    fn value_at_zero_order_hold() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&a, Value::U32(5)).unwrap();
        st.set_time(4_000);
        st.record(&a, Value::U32(9)).unwrap();

        assert_eq!(st.value_at(&a, 500).unwrap(), None); // before first sample
        assert_eq!(st.value_at(&a, 1_000).unwrap(), Some(&Value::U32(5)));
        assert_eq!(st.value_at(&a, 2_500).unwrap(), Some(&Value::U32(5))); // held
        assert_eq!(st.value_at(&a, 9_999).unwrap(), Some(&Value::U32(9)));
    }

    #[test]
    fn override_pins_value() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&a, Value::U32(5)).unwrap();
        st.set_override(&a, true).unwrap();
        st.set_time(2_000);
        st.record(&a, Value::U32(99)).unwrap(); // ignored
        assert_eq!(st.current_value(&a).unwrap(), Some(&Value::U32(5)));
    }

    #[test]
    fn per_signal_epsilon() {
        let a = id("cvar:dut:a");
        let mut cfg = StateTableConfig::default();
        cfg.signal_epsilon.insert(a.clone(), 0.5);
        let mut st = StateTable::with_config(cfg);
        st.register(a.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&a, Value::F32(1.0)).unwrap();
        st.set_time(2_000);
        st.record(&a, Value::F32(1.4)).unwrap(); // within 0.5 -> deduped
        assert_eq!(st.changes(&a).unwrap().len(), 1);
        st.set_time(3_000);
        st.record(&a, Value::F32(2.0)).unwrap(); // beyond 0.5 -> logged
        assert_eq!(st.changes(&a).unwrap().len(), 2);
    }

    #[test]
    fn unknown_signal_errors() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        assert!(matches!(
            st.record(&a, Value::U32(1)),
            Err(TableError::UnknownSignal(_))
        ));
    }

    #[test]
    fn log_stamps_current_sim_time_and_drains() {
        let mut st = StateTable::new();
        st.set_time(1_000);
        st.log(LogLevel::Warning, "member_a", "something odd");
        st.set_time(2_500);
        st.log(LogLevel::Error, "member_b", format!("code {}", 7));

        // Peek keeps the entries; timestamps are the table's sim time at log time.
        assert_eq!(st.logs().len(), 2);
        assert_eq!(st.logs()[0].time_us, 1_000);
        assert_eq!(st.logs()[0].level, LogLevel::Warning);
        assert_eq!(st.logs()[0].source, "member_a");
        assert_eq!(st.logs()[1].time_us, 2_500);
        assert_eq!(st.logs()[1].message, "code 7");

        // Drain empties the ring.
        let drained = st.take_logs();
        assert_eq!(drained.len(), 2);
        assert!(st.logs().is_empty());
    }

    #[test]
    fn log_ring_drops_oldest_beyond_capacity() {
        let cfg = StateTableConfig {
            log_capacity: 2,
            ..StateTableConfig::default()
        };
        let mut st = StateTable::with_config(cfg);
        for i in 0..5u64 {
            st.set_time(i * 1_000);
            st.log(LogLevel::Info, "src", format!("m{i}"));
        }
        assert_eq!(st.logs().len(), 2);
        assert_eq!(st.dropped_logs(), 3);
        assert_eq!(st.logs().front().unwrap().message, "m3");
        assert_eq!(st.logs().back().unwrap().message, "m4");
    }

    #[test]
    fn command_record_marks_dirty_mirror_does_not() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();

        // A mirror record does NOT mark dirty.
        st.set_time(1_000);
        st.record_mirror(&a, Value::U32(5)).unwrap();
        assert!(st.take_dirty("dut").is_empty());
        assert_eq!(st.current_value(&a).unwrap(), Some(&Value::U32(5)));

        // A command record marks dirty (drained exactly once).
        st.set_time(2_000);
        st.record(&a, Value::U32(9)).unwrap();
        assert_eq!(st.take_dirty("dut"), vec![a.clone()]);
        assert!(st.take_dirty("dut").is_empty());

        // Even a deduped (unchanged) command re-marks dirty: the driver keeps
        // re-asserting a routed value every tick.
        st.set_time(3_000);
        st.record(&a, Value::U32(9)).unwrap(); // unchanged -> no historian append
        assert_eq!(st.changes(&a).unwrap().len(), 2);
        assert_eq!(st.take_dirty("dut"), vec![a]);
    }

    #[test]
    fn pinned_ignores_mirror_and_flushes_every_tick() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();
        st.set_time(1_000);
        st.record_mirror(&a, Value::U32(5)).unwrap();

        // Pin at 5; it enters the pinned set (and is dirty once).
        st.set_override(&a, true).unwrap();
        assert_eq!(st.pinned("dut"), vec![a.clone()]);

        // A mirror record on a pinned entry is ignored — the sweep cannot un-pin
        // the view.
        st.set_time(2_000);
        st.record_mirror(&a, Value::U32(99)).unwrap();
        assert_eq!(st.current_value(&a).unwrap(), Some(&Value::U32(5)));

        // The pin persists across take_dirty drains: it must flush EVERY tick.
        st.take_dirty("dut");
        assert_eq!(st.pinned("dut"), vec![a]);
    }

    #[test]
    fn take_dirty_is_source_scoped() {
        let mut st = StateTable::new();
        let a = id("cvar:board_a:x");
        let b = id("cvar:board_b:y");
        st.register(a.clone(), None).unwrap();
        st.register(b.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&a, Value::U32(1)).unwrap();
        st.record(&b, Value::U32(2)).unwrap();

        // Draining board_a leaves board_b's dirt in place.
        assert_eq!(st.take_dirty("board_a"), vec![a]);
        assert_eq!(st.take_dirty("board_b"), vec![b]);
    }

    #[test]
    fn register_is_idempotent_but_rejects_unit_conflict() {
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), Some("counts")).unwrap();
        // Record some history, then re-register identically — a benign no-op that
        // must NOT wipe the entry or its change-log.
        st.set_time(1_000);
        st.record(&a, Value::U32(5)).unwrap();
        st.register(a.clone(), Some("counts")).unwrap(); // idempotent
        assert_eq!(st.len(), 1);
        assert_eq!(st.changes(&a).unwrap().len(), 1);
        assert_eq!(st.current_value(&a).unwrap(), Some(&Value::U32(5)));
        // A conflicting unit is a wiring bug.
        assert!(matches!(
            st.register(a.clone(), Some("volts")),
            Err(TableError::ConflictingUnit { .. })
        ));
        // None vs Some also conflicts.
        assert!(matches!(
            st.register(a.clone(), None),
            Err(TableError::ConflictingUnit { .. })
        ));
    }
}
