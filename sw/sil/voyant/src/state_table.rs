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

use crate::signal::{SignalId, Value};
use indexmap::IndexSet;
use std::collections::{HashMap, HashSet, VecDeque};
use std::time::Duration;
use thiserror::Error;

/// Default float change-detection epsilon ("moved, not noise" — D12).
const DEFAULT_EPSILON: f64 = 1e-3;
/// Default realtime retention window; fast mode overrides to `None` (unbounded).
const DEFAULT_RETENTION: Duration = Duration::from_secs(30);

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
}

impl Default for StateTableConfig {
    fn default() -> Self {
        Self {
            retention: Some(DEFAULT_RETENTION),
            signal_retention: HashMap::new(),
            epsilon: DEFAULT_EPSILON,
            signal_epsilon: HashMap::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum TableError {
    #[error("unknown signal: {0}")]
    UnknownSignal(SignalId),
    #[error("duplicate signal: {0}")]
    DuplicateSignal(SignalId),
    #[error("tick {requested_tick} for {signal} is before the retained window (oldest {oldest_available})")]
    OutOfWindow {
        signal: SignalId,
        requested_tick: u64,
        oldest_available: u64,
    },
}

pub struct StateTable {
    /// Registered signals, in insertion order (stable iteration / columnar dump).
    signals: IndexSet<SignalId>,
    /// Optional per-signal unit string (metadata).
    units: HashMap<SignalId, String>,
    /// Per-signal change-log: `(time_us, value)`, ascending, front-evicted.
    changes: HashMap<SignalId, VecDeque<(u64, Value)>>,
    /// Current value cache (O(1) latest).
    current: HashMap<SignalId, Value>,
    /// Signals whose `record` is ignored (injection pin).
    overrides: HashSet<SignalId>,
    /// Signals that have had samples evicted (so `value_at` can report OutOfWindow).
    evicted: HashSet<SignalId>,
    /// Current sim time (microseconds); the timestamp for `record`.
    current_time_us: u64,
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
            changes: HashMap::new(),
            current: HashMap::new(),
            overrides: HashSet::new(),
            evicted: HashSet::new(),
            current_time_us: 0,
            config,
        }
    }

    /// Set the current sim time (used to timestamp subsequent records).
    pub fn set_time(&mut self, time_us: u64) {
        self.current_time_us = time_us;
    }

    /// Register a signal (optionally with a unit). Errors if already registered.
    pub fn register(&mut self, id: SignalId, unit: Option<&str>) -> Result<(), TableError> {
        if self.signals.contains(&id) {
            return Err(TableError::DuplicateSignal(id));
        }
        if let Some(u) = unit {
            self.units.insert(id.clone(), u.to_string());
        }
        self.changes.insert(id.clone(), VecDeque::new());
        self.signals.insert(id);
        Ok(())
    }

    /// Record a value at the current time. Skips if the signal is overridden or
    /// the value is unchanged (within epsilon). Errors if not registered.
    pub fn record(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        self.ensure(id)?;
        if self.overrides.contains(id) {
            return Ok(());
        }
        if let Some(cur) = self.current.get(id) {
            if cur.approx_eq(&value, self.epsilon_for(id)) {
                return Ok(());
            }
        }
        self.append(id, value);
        Ok(())
    }

    /// Record unconditionally (bypasses change-detection and overrides).
    pub fn force_record(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        self.ensure(id)?;
        self.append(id, value);
        Ok(())
    }

    /// Current (last-recorded) value, O(1). `None` if never recorded.
    pub fn current_value(&self, id: &SignalId) -> Result<Option<&Value>, TableError> {
        self.ensure(id)?;
        Ok(self.current.get(id))
    }

    /// Value in effect at `time_us` (zero-order hold: the last sample at or
    /// before it), O(log n). `None` if before the first sample and nothing was
    /// evicted; `OutOfWindow` if it fell out of the retained window.
    pub fn value_at(&self, id: &SignalId, time_us: u64) -> Result<Option<&Value>, TableError> {
        self.ensure(id)?;
        let dq = &self.changes[id];
        if dq.is_empty() {
            return Ok(None);
        }
        let idx = dq.partition_point(|&(t, _)| t <= time_us);
        if idx == 0 {
            // Requested time precedes all retained samples.
            if self.evicted.contains(id) {
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

    /// Pin/unpin a signal (overridden signals ignore `record`).
    pub fn set_override(&mut self, id: &SignalId, on: bool) -> Result<(), TableError> {
        self.ensure(id)?;
        if on {
            self.overrides.insert(id.clone());
        } else {
            self.overrides.remove(id);
        }
        Ok(())
    }

    /// The change-log for a signal (timestamped samples), for dump/inspection.
    pub fn changes(&self, id: &SignalId) -> Option<&VecDeque<(u64, Value)>> {
        self.changes.get(id)
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

    fn ensure(&self, id: &SignalId) -> Result<(), TableError> {
        if self.signals.contains(id) {
            Ok(())
        } else {
            Err(TableError::UnknownSignal(id.clone()))
        }
    }

    fn epsilon_for(&self, id: &SignalId) -> f64 {
        self.config
            .signal_epsilon
            .get(id)
            .copied()
            .unwrap_or(self.config.epsilon)
    }

    fn append(&mut self, id: &SignalId, value: Value) {
        let t = self.current_time_us;
        self.current.insert(id.clone(), value.clone());
        self.changes.get_mut(id).unwrap().push_back((t, value));
        self.evict(id);
    }

    /// Drop samples older than the retention window, keeping the one sample that
    /// is still "active" at the cutoff (ZOH needs it).
    fn evict(&mut self, id: &SignalId) {
        let window = self
            .config
            .signal_retention
            .get(id)
            .copied()
            .or(self.config.retention);
        let Some(window) = window else { return };
        let cutoff = self
            .current_time_us
            .saturating_sub(window.as_micros() as u64);
        let dq = self.changes.get_mut(id).unwrap();
        let mut dropped = false;
        while dq.len() > 1 && dq[1].0 <= cutoff {
            dq.pop_front();
            dropped = true;
        }
        if dropped {
            self.evicted.insert(id.clone());
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
        let a = id("cvar:pcs_bldc:a");
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
        let a = id("cvar:pcs_bldc:a");
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
        let a = id("cvar:pcs_bldc:a");
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
        let a = id("cvar:pcs_bldc:a");
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
    fn unknown_and_duplicate() {
        let mut st = StateTable::new();
        let a = id("cvar:pcs_bldc:a");
        assert!(matches!(
            st.record(&a, Value::U32(1)),
            Err(TableError::UnknownSignal(_))
        ));
        st.register(a.clone(), None).unwrap();
        assert!(matches!(
            st.register(a.clone(), None),
            Err(TableError::DuplicateSignal(_))
        ));
    }
}
