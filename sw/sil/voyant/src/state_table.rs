//! The State Table: signal registry + per-signal change-logged history +
//! current-value cache + bounded retention.
//!
//! Pure data — no FFI, no DWARF. Fed by [`StateTable::record`], queried by
//! [`StateTable::current_value`] (O(1)) / [`StateTable::value_at`] (O(log n),
//! zero-order-hold). Each signal *is* its own historian (D12): a record is stored
//! only when the value moves past the signal's epsilon (default 1e-3 for floats;
//! exact otherwise). Writes are one-shot, last-writer-wins — a value persists
//! exactly when nothing else writes that signal. Retention evicts old samples by
//! time window (unbounded in fast mode).

use crate::log::{LogEntry, LogLevel, LogRing};
use crate::signal::{ParseError, SignalId, Value};
use crate::unit::{UnitError, UnitRegistry};
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

/// A dense **typed sub-column**: parallel deques of ascending timestamps and
/// native values (`times[i]` is when the signal became `vals[i]`), front-evicted
/// as a pair. The hot scalar storage — 8 B + `size_of::<T>()` per sample vs the
/// old `(u64, Value)` pair's ~40 B.
struct TypedCol<T> {
    times: VecDeque<u64>,
    vals: VecDeque<T>,
}

impl<T: Copy> TypedCol<T> {
    fn new() -> Self {
        Self {
            times: VecDeque::new(),
            vals: VecDeque::new(),
        }
    }

    fn len(&self) -> usize {
        self.times.len()
    }

    fn push(&mut self, t: u64, v: T) {
        self.times.push_back(t);
        self.vals.push_back(v);
    }

    /// The most-recent value (column tail), for the native epsilon-dedup compare.
    fn tail(&self) -> Option<T> {
        self.vals.back().copied()
    }

    /// Drop samples strictly older than `cutoff`, keeping the one sample still
    /// "active" at the cutoff (ZOH needs it). Returns whether anything dropped.
    fn evict(&mut self, cutoff: u64) -> bool {
        let mut dropped = false;
        while (self.times.len() > 1) && (self.times[1] <= cutoff) {
            self.times.pop_front();
            self.vals.pop_front();
            dropped = true;
        }
        dropped
    }

    /// ZOH partition point: the count of samples at or before `time_us`.
    fn zoh_index(&self, time_us: u64) -> usize {
        self.times.partition_point(|&t| t <= time_us)
    }
}

/// One signal's **columnar historian**. The kind is fixed at the first record and
/// immutable thereafter: scalar variants get a dense [`TypedCol`]; `Enum`/`Bytes`
/// live in the boxed `(time_us, Value)` column. A later record of a mismatched
/// variant is a bug (one signal has exactly one type for its lifetime), rejected by
/// [`Column::push_value`] — no migration. Scalars are the 99% hot case.
enum Column {
    /// No record yet — kind undetermined.
    Empty,
    F32(TypedCol<f32>),
    F64(TypedCol<f64>),
    I32(TypedCol<i32>),
    U32(TypedCol<u32>),
    U64(TypedCol<u64>),
    Bool(TypedCol<bool>),
    /// Structured signals (`Enum`/`Bytes`). Seeded to one variant at first record
    /// and strict thereafter — they are distinct types under the one-type rule.
    Boxed(VecDeque<(u64, Value)>),
}

/// A zero-order-hold lookup outcome (see [`Column::zoh`]).
enum Zoh {
    /// Nothing recorded yet.
    Empty,
    /// The requested time precedes the oldest retained sample (its timestamp).
    Before(u64),
    /// The value held at the requested time.
    At(Value),
}

impl Column {
    /// Timestamp of the newest sample, O(1); `None` for an empty column.
    fn last_time(&self) -> Option<u64> {
        match self {
            Column::Empty => None,
            Column::F32(c) => c.times.back().copied(),
            Column::F64(c) => c.times.back().copied(),
            Column::I32(c) => c.times.back().copied(),
            Column::U32(c) => c.times.back().copied(),
            Column::U64(c) => c.times.back().copied(),
            Column::Bool(c) => c.times.back().copied(),
            Column::Boxed(c) => c.back().map(|(t, _)| *t),
        }
    }

    /// The column's established kind name (`"Empty"` before the first record) — for
    /// the unit-ask non-float-column error.
    fn kind_name(&self) -> &'static str {
        match self {
            Column::Empty => "Empty",
            Column::F32(_) => "F32",
            Column::F64(_) => "F64",
            Column::I32(_) => "I32",
            Column::U32(_) => "U32",
            Column::U64(_) => "U64",
            Column::Bool(_) => "Bool",
            Column::Boxed(dq) => boxed_kind_name(dq),
        }
    }

    /// Seed an empty column from the incoming value, choosing the column kind by
    /// its variant (scalars → a typed column; `Enum`/`Bytes` → boxed).
    fn seed(t: u64, value: Value) -> Column {
        fn one<T: Copy>(t: u64, x: T) -> TypedCol<T> {
            let mut c = TypedCol::new();
            c.push(t, x);
            c
        }
        match value {
            Value::F32(x) => Column::F32(one(t, x)),
            Value::F64(x) => Column::F64(one(t, x)),
            Value::I32(x) => Column::I32(one(t, x)),
            Value::U32(x) => Column::U32(one(t, x)),
            Value::U64(x) => Column::U64(one(t, x)),
            Value::Bool(x) => Column::Bool(one(t, x)),
            v @ (Value::Enum(_) | Value::Bytes(_)) => {
                let mut dq = VecDeque::new();
                dq.push_back((t, v));
                Column::Boxed(dq)
            }
        }
    }

    /// Materialize the change-log as owned `(time, Value)` pairs (for dump /
    /// inspection via [`StateTable::changes`]).
    fn to_pairs(&self) -> Vec<(u64, Value)> {
        fn zip<T: Copy>(c: &TypedCol<T>, wrap: impl Fn(T) -> Value) -> Vec<(u64, Value)> {
            c.times.iter().copied().zip(c.vals.iter().copied()).map(|(t, v)| (t, wrap(v))).collect()
        }
        match self {
            Column::Empty => Vec::new(),
            Column::Boxed(dq) => dq.iter().cloned().collect(),
            Column::F32(c) => zip(c, Value::F32),
            Column::F64(c) => zip(c, Value::F64),
            Column::I32(c) => zip(c, Value::I32),
            Column::U32(c) => zip(c, Value::U32),
            Column::U64(c) => zip(c, Value::U64),
            Column::Bool(c) => zip(c, Value::Bool),
        }
    }

    /// Append a `Value` at `t`. Empty → fixes the kind; matching kind → appends. A
    /// variant that does not match the established kind is a bug (one signal has
    /// exactly one type for its lifetime) and is rejected with
    /// `Err((column_kind, offending_variant))`, column left untouched — never
    /// panics, migrates, or coerces. `Boxed` is strict per structured variant (an
    /// `Enum`-seeded column rejects `Bytes` and vice versa).
    fn push_value(&mut self, t: u64, value: Value) -> Result<(), (&'static str, &'static str)> {
        match self {
            Column::Empty => {
                *self = Column::seed(t, value);
                Ok(())
            }
            Column::F32(c) => match value {
                Value::F32(x) => {
                    c.push(t, x);
                    Ok(())
                }
                v => Err(("F32", value_variant_name(&v))),
            },
            Column::F64(c) => match value {
                Value::F64(x) => {
                    c.push(t, x);
                    Ok(())
                }
                v => Err(("F64", value_variant_name(&v))),
            },
            Column::I32(c) => match value {
                Value::I32(x) => {
                    c.push(t, x);
                    Ok(())
                }
                v => Err(("I32", value_variant_name(&v))),
            },
            Column::U32(c) => match value {
                Value::U32(x) => {
                    c.push(t, x);
                    Ok(())
                }
                v => Err(("U32", value_variant_name(&v))),
            },
            Column::U64(c) => match value {
                Value::U64(x) => {
                    c.push(t, x);
                    Ok(())
                }
                v => Err(("U64", value_variant_name(&v))),
            },
            Column::Bool(c) => match value {
                Value::Bool(x) => {
                    c.push(t, x);
                    Ok(())
                }
                v => Err(("Bool", value_variant_name(&v))),
            },
            Column::Boxed(dq) => {
                // Established kind = the existing entries' variant (never empty once seeded).
                let established = boxed_kind_name(dq);
                let matches = matches!(
                    (established, &value),
                    ("Enum", Value::Enum(_)) | ("Bytes", Value::Bytes(_))
                );
                if matches {
                    dq.push_back((t, value));
                    Ok(())
                } else {
                    Err((established, value_variant_name(&value)))
                }
            }
        }
    }

    /// Evict samples older than `cutoff` (ZOH-preserving). Returns whether anything
    /// was dropped.
    fn evict(&mut self, cutoff: u64) -> bool {
        match self {
            Column::Empty => false,
            Column::F32(c) => c.evict(cutoff),
            Column::F64(c) => c.evict(cutoff),
            Column::I32(c) => c.evict(cutoff),
            Column::U32(c) => c.evict(cutoff),
            Column::U64(c) => c.evict(cutoff),
            Column::Bool(c) => c.evict(cutoff),
            Column::Boxed(dq) => {
                let mut dropped = false;
                while (dq.len() > 1) && (dq[1].0 <= cutoff) {
                    dq.pop_front();
                    dropped = true;
                }
                dropped
            }
        }
    }

    /// Zero-order-hold lookup: the value in effect at `time_us`.
    fn zoh(&self, time_us: u64) -> Zoh {
        fn typed<T: Copy>(c: &TypedCol<T>, time_us: u64, wrap: impl Fn(T) -> Value) -> Zoh {
            if c.len() == 0 {
                return Zoh::Empty;
            }
            let idx = c.zoh_index(time_us);
            if idx == 0 {
                Zoh::Before(c.times[0])
            } else {
                Zoh::At(wrap(c.vals[idx - 1]))
            }
        }
        match self {
            Column::Empty => Zoh::Empty,
            Column::F32(c) => typed(c, time_us, Value::F32),
            Column::F64(c) => typed(c, time_us, Value::F64),
            Column::I32(c) => typed(c, time_us, Value::I32),
            Column::U32(c) => typed(c, time_us, Value::U32),
            Column::U64(c) => typed(c, time_us, Value::U64),
            Column::Bool(c) => typed(c, time_us, Value::Bool),
            Column::Boxed(dq) => {
                if dq.is_empty() {
                    return Zoh::Empty;
                }
                let idx = dq.partition_point(|&(t, _)| t <= time_us);
                if idx == 0 {
                    Zoh::Before(dq[0].0)
                } else {
                    Zoh::At(dq[idx - 1].1.clone())
                }
            }
        }
    }
}

/// The structured kind (`"Enum"`/`"Bytes"`) of a seeded boxed column, for the
/// type-mismatch error. All entries share the variant, so the tail is authoritative.
fn boxed_kind_name(dq: &VecDeque<(u64, Value)>) -> &'static str {
    match dq.back() {
        Some((_, Value::Bytes(_))) => "Bytes",
        _ => "Enum",
    }
}

/// Split a trailing **unit ask** off an id string. A trailing `[X]` where `X` is
/// non-empty and NOT all ASCII digits is a per-call unit ask: returns `(id without
/// the bracket, Some(X))`. An all-digit bracket (`counts[6]`) is a cvar array index
/// — left on the id, `None`. Unit names never start with a digit, so the two never
/// collide. No qualifying trailing bracket → `(id, None)`.
fn split_unit_ask(id: &str) -> (&str, Option<&str>) {
    if let Some(head) = id.strip_suffix(']') {
        if let Some(open) = head.rfind('[') {
            let inner = &head[open + 1..];
            let is_ask = !inner.is_empty() && !inner.bytes().all(|b| b.is_ascii_digit());
            if is_ask {
                return (&id[..open], Some(inner));
            }
        }
    }
    (id, None)
}

/// The `Value` variant name, for the type-mismatch error.
fn value_variant_name(v: &Value) -> &'static str {
    match v {
        Value::F32(_) => "F32",
        Value::F64(_) => "F64",
        Value::I32(_) => "I32",
        Value::U32(_) => "U32",
        Value::U64(_) => "U64",
        Value::Bool(_) => "Bool",
        Value::Enum(_) => "Enum",
        Value::Bytes(_) => "Bytes",
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
    #[error("type mismatch on {signal}: column is {column_kind} but got a {offending} value (one signal has exactly one Value type for its lifetime — this is a wiring/model bug)")]
    TypeMismatch {
        signal: SignalId,
        column_kind: &'static str,
        offending: &'static str,
    },
}

/// Error from the **string-keyed** table API ([`write`](StateTable::write) /
/// [`read`](StateTable::read)): either the id string didn't parse, or the
/// underlying table operation failed (unregistered signal, type mismatch). Never
/// panics — the uniform scenario entry point returns every failure as an `Err`.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum AccessError {
    #[error("bad signal id: {0}")]
    Parse(#[from] ParseError),
    #[error(transparent)]
    Table(#[from] TableError),
    /// A bracket unit ask named a unit absent from the unit registry (a typo'd ask
    /// unit, or a signal whose canonical unit was never registered for conversion).
    #[error("unknown unit {0:?} (not in the unit registry)")]
    UnknownUnit(String),
    /// The bracket ask unit and the signal's canonical unit are different dimensions
    /// (`angle[V]`) — a wiring error, never a silent pass-through.
    #[error("dimension mismatch: ask unit {ask:?} vs canonical unit {canonical:?}")]
    DimensionMismatch { ask: String, canonical: String },
    /// A unit ask on a signal registered with no canonical unit — nothing to convert
    /// against.
    #[error("unit ask on {0} which has no canonical unit (registered without one)")]
    UnitlessSignal(SignalId),
    /// A unit-ask `write` was handed a non-float `Value` — conversion needs a float.
    #[error("unit ask on {signal}: value is a {offending}, not a float")]
    NonFloatValue { signal: SignalId, offending: &'static str },
    /// A unit ask hit a non-float column — the stored kind (read) or the seeded
    /// column kind (write) is neither `F32` nor `F64`, so conversion is undefined.
    #[error("unit ask on {signal}: column is {column_kind}, not F32/F64")]
    NonFloatColumn { signal: SignalId, column_kind: &'static str },
}

pub struct StateTable {
    /// Registered signals in insertion order. The [`IndexSet`] gives each signal a
    /// **stable dense index** (append-only — never removed), the key for all hot
    /// per-signal storage below. The string-keyed API resolves an id to its index
    /// once ([`resolve_index`](Self::resolve_index)) and delegates to the `*_at`
    /// internals, keeping per-record string hashing off hot paths.
    signals: IndexSet<SignalId>,
    /// Optional per-signal **canonical** unit string (metadata; cold — set only at
    /// register). Every stored sample of a signal is in this unit; a bracket unit
    /// ask on [`write`](Self::write) / [`read`](Self::read) converts to/from it.
    units: HashMap<SignalId, String>,
    /// The runtime unit-conversion table (built-ins + [`add_unit`](Self::add_unit)),
    /// consulted only on the string API's bracket unit ask.
    unit_registry: UnitRegistry,
    /// Per-signal **columnar historian**, by signal index (scalar → dense
    /// [`TypedCol`], `Enum`/`Bytes` → boxed deque). Ascending, front-evicted; kind
    /// is fixed at the FIRST record and immutable (a later mismatch is rejected with
    /// [`TableError::TypeMismatch`]).
    columns: Vec<Column>,
    /// Current value cache, by signal index (O(1) latest; `None` = never recorded).
    current: Vec<Option<Value>>,
    /// Resolved change-detection epsilon, by signal index (per-signal override else
    /// global — resolved at register so the hot `record` path never hashes config).
    epsilon: Vec<f64>,
    /// **Command-write dirty set**, by signal index: indices a framework command
    /// wrote this cycle (`record` / `force_record`) — *not* mirror sweeps
    /// (`record_mirror`). A firmware member drains its own namespace's dirt each
    /// tick to know which `cvar`s to flush back into firmware memory.
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

/// Generate a **typed mirror-record fast lane** (`record_mirror_<t>_at`) for one
/// scalar kind: compare the native value against the column tail directly (no
/// `Value` construction), append on change, keep `current`/retention identical to
/// [`StateTable::record_mirror_at`]. A wrong-kind column defers to the generic
/// path, which rejects it ([`TableError::TypeMismatch`] — one type per signal).
/// `$eq` is the dedup predicate (float: absolute deadband; discrete: exact).
macro_rules! typed_mirror_lane {
    ($name:ident, $t:ty, $variant:ident, $eq:expr) => {
        pub(crate) fn $name(&mut self, idx: usize, x: $t) -> Result<(), TableError> {
            match &self.columns[idx] {
                Column::$variant(c) => {
                    if let Some(prev) = c.tail() {
                        let eq: fn($t, $t, f64) -> bool = $eq;
                        if eq(prev, x, self.epsilon[idx]) {
                            return Ok(()); // within epsilon -> deduped
                        }
                    }
                }
                Column::Empty => {}
                // Different kind: defer to the generic path, which rejects it.
                _ => {
                    return self.record_inner(idx, Value::$variant(x), false);
                }
            }
            let t = self.current_time_us;
            self.current[idx] = Some(Value::$variant(x));
            match &mut self.columns[idx] {
                Column::Empty => {
                    let mut c = TypedCol::new();
                    c.push(t, x);
                    self.columns[idx] = Column::$variant(c);
                }
                Column::$variant(c) => c.push(t, x),
                _ => unreachable!("compare arm above already excluded other kinds"),
            }
            self.evict(idx);
            Ok(())
        }
    };
}

impl StateTable {
    pub fn new() -> Self {
        Self::with_config(StateTableConfig::default())
    }

    pub fn with_config(config: StateTableConfig) -> Self {
        Self {
            signals: IndexSet::new(),
            units: HashMap::new(),
            unit_registry: UnitRegistry::new(),
            columns: Vec::new(),
            current: Vec::new(),
            epsilon: Vec::new(),
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
    /// **Idempotent**: re-registering with an *identical* unit is a benign no-op — a
    /// member (e.g. firmware across a reboot) re-registers its signals, and history
    /// spans member lifetimes, so the entry + change-log are preserved. A
    /// *conflicting* unit is a wiring bug ([`TableError::ConflictingUnit`]).
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
        // Resolve epsilon once, so the hot `record` path never hashes config.
        let eps = self
            .config
            .signal_epsilon
            .get(&id)
            .copied()
            .unwrap_or(self.config.epsilon);
        // Push dense slots BEFORE inserting into the index set: the new signal's
        // index (== pre-push length) must line up with its slot.
        self.columns.push(Column::Empty);
        self.current.push(None);
        self.epsilon.push(eps);
        self.signals.insert(id);
        Ok(())
    }

    /// Extend the unit-conversion registry at runtime (see [`UnitRegistry::add_unit`]).
    /// Idempotent for an identical definition; a conflicting redefinition or a
    /// digit-leading name is a [`UnitError`].
    pub fn add_unit(
        &mut self,
        name: &str,
        dimension: &str,
        scale: f64,
        offset: f64,
    ) -> Result<(), UnitError> {
        self.unit_registry.add_unit(name, dimension, scale, offset)
    }

    /// Resolve a signal id to its stable dense index, or `None` if unregistered.
    /// Consumers resolve **once** (at registration / route validation) and then use
    /// the index-keyed hot paths, avoiding per-tick string hashing.
    pub(crate) fn resolve_index(&self, id: &SignalId) -> Option<usize> {
        self.signals.get_index_of(id)
    }

    /// The signal at dense index `idx`, or `None` if out of range. Inverse of
    /// [`resolve_index`](Self::resolve_index): lets a consumer that memoized an index
    /// verify it still names the *expected* signal in **this** table (guards against
    /// a `RouteTable` propagated against a different `StateTable`).
    pub(crate) fn id_at(&self, idx: usize) -> Option<&SignalId> {
        self.signals.get_index(idx)
    }

    /// Record a **command-written** value (route, test, or model output) at the
    /// current time. Marks the signal dirty. Skips the historian append if unchanged,
    /// but the dirty mark stands whenever the command applied (an unchanged re-command
    /// still means the framework is driving it, so it must be re-asserted into firmware
    /// memory each tick). Errors if not registered.
    pub fn record(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        self.record_inner(idx, value, true)
    }

    // --- string-keyed scenario API ---------------------------------------
    // Write/read a signal by id string (one uniform call shape, no typed handle): parse
    // the id, delegate to the typed primitive above; a bad/unregistered/mistyped id
    // returns an `Err`, never a panic. A trailing non-numeric bracket (`angle[deg]`)
    // is a per-call **unit ask** converted at this boundary against the signal's
    // canonical unit; an all-digit bracket (`counts[6]`) is a cvar array index and
    // stays part of the id. The ask is never stored.

    /// Record a value by **string id** — parse then [`record`](Self::record).
    /// Literals coerce via [`Value`]'s `From` impls (`write(id, true)`). A bracket
    /// unit ask converts ask→canonical, then records **coerced to the column's kind**
    /// (`F32` column → `F32`, `F64`/empty → `F64`) so the one-type-per-signal rule
    /// holds regardless of the ask unit.
    pub fn write(&mut self, id: &str, value: impl Into<Value>) -> Result<(), AccessError> {
        let (id_str, ask) = split_unit_ask(id);
        let sid = SignalId::parse(id_str)?;
        let value = value.into();
        let Some(ask) = ask else {
            self.record(&sid, value)?;
            return Ok(());
        };
        let idx = self.ensure(&sid)?;
        let canonical = self.canonical_unit(&sid)?.to_string();
        self.validate_unit_ask(ask, &canonical)?;
        let incoming = value.as_f64().ok_or_else(|| AccessError::NonFloatValue {
            signal: sid.clone(),
            offending: value_variant_name(&value),
        })?;
        let canon = self
            .unit_registry
            .convert(incoming, ask, &canonical)
            .expect("units pre-validated by validate_unit_ask");
        let coerced = match &self.columns[idx] {
            Column::Empty | Column::F64(_) => Value::F64(canon),
            Column::F32(_) => Value::F32(canon as f32),
            other => {
                return Err(AccessError::NonFloatColumn {
                    signal: sid.clone(),
                    column_kind: other.kind_name(),
                })
            }
        };
        self.record(&sid, coerced)?;
        Ok(())
    }

    /// Read the current value by **string id** — parse then
    /// [`current_value`](Self::current_value). `Ok(None)` = registered but never
    /// recorded. A bracket unit ask converts the canonical current value canonical→ask
    /// and returns `Some(Value::F64(..))`; `Ok(None)` still means never recorded.
    pub fn read(&self, id: &str) -> Result<Option<Value>, AccessError> {
        let (id_str, ask) = split_unit_ask(id);
        let sid = SignalId::parse(id_str)?;
        let Some(ask) = ask else {
            return Ok(self.current_value(&sid)?);
        };
        self.ensure(&sid)?;
        let canonical = self.canonical_unit(&sid)?.to_string();
        self.validate_unit_ask(ask, &canonical)?;
        match self.current_value(&sid)? {
            None => Ok(None),
            Some(v) => {
                let stored = v.as_f64().ok_or_else(|| AccessError::NonFloatColumn {
                    signal: sid.clone(),
                    column_kind: value_variant_name(&v),
                })?;
                let out = self
                    .unit_registry
                    .convert(stored, &canonical, ask)
                    .expect("units pre-validated by validate_unit_ask");
                Ok(Some(Value::F64(out)))
            }
        }
    }

    /// The signal's canonical unit string, or [`AccessError::UnitlessSignal`] if it
    /// registered without one. The caller must first [`ensure`](Self::ensure) the
    /// signal is registered (an unregistered signal is an [`AccessError::Table`]
    /// `UnknownSignal`, not unitless).
    fn canonical_unit(&self, sid: &SignalId) -> Result<&str, AccessError> {
        self.units
            .get(sid)
            .map(String::as_str)
            .ok_or_else(|| AccessError::UnitlessSignal(sid.clone()))
    }

    /// Validate a unit ask against a signal's canonical unit: both must be registered
    /// in the unit registry and share a dimension. Precise, distinct errors —
    /// [`AccessError::UnknownUnit`] (names the offending unit) or
    /// [`AccessError::DimensionMismatch`].
    fn validate_unit_ask(&self, ask: &str, canonical: &str) -> Result<(), AccessError> {
        let canon_dim = self
            .unit_registry
            .dimension_of(canonical)
            .ok_or_else(|| AccessError::UnknownUnit(canonical.to_string()))?;
        let ask_dim = self
            .unit_registry
            .dimension_of(ask)
            .ok_or_else(|| AccessError::UnknownUnit(ask.to_string()))?;
        if canon_dim != ask_dim {
            return Err(AccessError::DimensionMismatch {
                ask: ask.to_string(),
                canonical: canonical.to_string(),
            });
        }
        Ok(())
    }

    /// Record a **mirror** value (a firmware member's end-of-tick sweep of memory
    /// into the table). Same dedup/historian as [`record`](Self::record) but does
    /// **not** mark dirty (tracking what memory already holds, not commanding a write
    /// back).
    pub fn record_mirror(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        self.record_inner(idx, value, false)
    }

    /// **Index-keyed mirror record** — the sweep hot path. Like
    /// [`record_mirror`](Self::record_mirror) but keyed by a pre-resolved dense index
    /// (from [`resolve_index`](Self::resolve_index)), so the sweep never hashes a
    /// `SignalId`.
    pub(crate) fn record_mirror_at(&mut self, idx: usize, value: Value) -> Result<(), TableError> {
        self.record_inner(idx, value, false)
    }

    // **Typed mirror-record fast lanes** — the sweep's scalar decode path. Each takes
    // a native scalar (no boxed `Value`) so changing leaves compare against the column
    // tail and append natively, no `Value` construction on the compare path.
    // Behaviorally identical to `record_mirror_at(idx, Value::<V>(x))`, including the
    // wrong-kind `TypeMismatch`. Enum/Bytes leaves have no fast lane.
    typed_mirror_lane!(record_mirror_f32_at, f32, F32, |a, b, e| ((a as f64) - (b as f64)).abs() <= e);
    typed_mirror_lane!(record_mirror_f64_at, f64, F64, |a, b, e| (a - b).abs() <= e);
    typed_mirror_lane!(record_mirror_i32_at, i32, I32, |a, b, _e| a == b);
    typed_mirror_lane!(record_mirror_u32_at, u32, U32, |a, b, _e| a == b);
    typed_mirror_lane!(record_mirror_u64_at, u64, U64, |a, b, _e| a == b);
    typed_mirror_lane!(record_mirror_bool_at, bool, Bool, |a, b, _e| a == b);

    /// **Index-keyed command record** — the route-propagation hot path. Like
    /// [`record`](Self::record) but keyed by a pre-resolved dense index (from
    /// [`resolve_index`](Self::resolve_index)).
    pub(crate) fn record_at(&mut self, idx: usize, value: Value) -> Result<(), TableError> {
        self.record_inner(idx, value, true)
    }

    fn record_inner(&mut self, idx: usize, value: Value, command: bool) -> Result<(), TableError> {
        if command {
            self.dirty.insert(idx);
        }
        if let Some(cur) = &self.current[idx] {
            if cur.approx_eq(&value, self.epsilon[idx]) {
                return Ok(());
            }
        }
        self.append(idx, value)
    }

    /// Record unconditionally (bypasses change-detection). Counts as a command write
    /// (marks the signal dirty).
    pub fn force_record(&mut self, id: &SignalId, value: Value) -> Result<(), TableError> {
        let idx = self.ensure(id)?;
        self.dirty.insert(idx);
        self.append(idx, value)
    }

    /// Current (last-recorded) value, O(1). `None` if never recorded. Returned **by
    /// value** — columnar storage can't hand out a `&Value` (D12 semantics intact).
    pub fn current_value(&self, id: &SignalId) -> Result<Option<Value>, TableError> {
        let idx = self.ensure(id)?;
        Ok(self.current[idx].clone())
    }

    /// Index-keyed current value (route propagation / port-cache fill hot paths).
    /// `idx` must come from [`resolve_index`](Self::resolve_index). By value (see
    /// [`current_value`](Self::current_value)).
    pub(crate) fn current_value_at(&self, idx: usize) -> Option<Value> {
        self.current[idx].clone()
    }

    /// Sim time of a signal's most recent **accepted change** (its change-log
    /// tail), O(1). `Ok(None)` = registered but never recorded. Freshness compares
    /// change times: an epsilon-deduped re-record of the same value does not
    /// advance this. Pairs with [`current_value`](Self::current_value) for
    /// last-writer-wins between alternative input signals.
    pub fn last_change_us(&self, id: &SignalId) -> Result<Option<u64>, TableError> {
        let idx = self.ensure(id)?;
        Ok(self.columns[idx].last_time())
    }

    /// Value in effect at `time_us` (zero-order hold: the last sample at or
    /// before it), O(log n), reconstructed **by value** from the typed column.
    /// `None` if before the first sample and nothing was evicted; `OutOfWindow` if
    /// it fell out of the retained window.
    pub fn value_at(&self, id: &SignalId, time_us: u64) -> Result<Option<Value>, TableError> {
        let si = self.ensure(id)?;
        match self.columns[si].zoh(time_us) {
            Zoh::Empty => Ok(None),
            Zoh::Before(oldest_available) => {
                // Requested time precedes all retained samples.
                if self.evicted.contains(&si) {
                    Err(TableError::OutOfWindow {
                        signal: id.clone(),
                        requested_tick: time_us,
                        oldest_available,
                    })
                } else {
                    Ok(None)
                }
            }
            Zoh::At(v) => Ok(Some(v)),
        }
    }

    /// Remove and return the dirty ids whose `<source>` segment equals `source`
    /// (a member drains its **own** namespace; other members' dirt is left in
    /// place for their own drain). Deterministic order (sorted by id string).
    pub fn take_dirty(&mut self, source: &str) -> Vec<SignalId> {
        // Dirty indices belonging to `source`, removed then materialized as ids
        // (deterministic, sorted). The dirty set is small, so this stays cheap.
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

    /// A signal's registered canonical unit, if any (`None` = registered without
    /// one). Cold metadata — for trace export ([`crate::trace`]) and inspection.
    pub fn unit_of(&self, id: &SignalId) -> Option<&str> {
        self.units.get(id).map(String::as_str)
    }

    /// The change-log for a signal (timestamped samples), for dump/inspection.
    /// **Materializing**: columnar storage has no `(u64, Value)` deque to borrow, so
    /// it reconstructs owned pairs (a cold path). `None` if unregistered; empty `Vec`
    /// if registered but never recorded.
    pub fn changes(&self, id: &SignalId) -> Option<Vec<(u64, Value)>> {
        self.signals.get_index_of(id).map(|idx| self.columns[idx].to_pairs())
    }

    /// Emit a log entry stamped with the table's **current sim time** — the caller
    /// supplies only severity, a `source` tag, and a message, never the timestamp, so
    /// members can't fake sim time or perturb behaviour (determinism, D7). The ring
    /// drops the oldest entry when full.
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

    fn append(&mut self, idx: usize, value: Value) -> Result<(), TableError> {
        let t = self.current_time_us;
        // Append into the typed column. A variant mismatching the column's kind is a
        // bug (one type per signal), rejected with column + `current` untouched (fail
        // loud, corrupt nothing); the caller surfaces the error.
        match self.columns[idx].push_value(t, value.clone()) {
            Ok(()) => {
                self.current[idx] = Some(value);
                self.evict(idx);
                Ok(())
            }
            Err((column_kind, offending)) => Err(TableError::TypeMismatch {
                signal: self.signals.get_index(idx).unwrap().clone(),
                column_kind,
                offending,
            }),
        }
    }

    /// Drop samples older than the retention window, keeping the one sample that
    /// is still "active" at the cutoff (ZOH needs it).
    fn evict(&mut self, idx: usize) {
        // Per-signal retention override (keyed by id) else global; resolve the window
        // first, then mutate the dense slots (disjoint borrows).
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
        if self.columns[idx].evict(cutoff) {
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
        assert_eq!(st.current_value(&a).unwrap(), Some(Value::U32(9)));
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
        assert_eq!(st.value_at(&a, 1_000).unwrap(), Some(Value::U32(5)));
        assert_eq!(st.value_at(&a, 2_500).unwrap(), Some(Value::U32(5))); // held
        assert_eq!(st.value_at(&a, 9_999).unwrap(), Some(Value::U32(9)));
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
        assert_eq!(st.current_value(&a).unwrap(), Some(Value::U32(5)));

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
        assert_eq!(st.current_value(&a).unwrap(), Some(Value::U32(5)));
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

    // --- columnar historian ------------------------------------------------

    #[test]
    fn columns_type_by_first_record_across_all_scalar_kinds() {
        // The column's value kind is fixed at the first record; every scalar
        // variant round-trips exactly through current_value / changes / value_at.
        let cases: Vec<(&str, Value)> = vec![
            ("f32", Value::F32(1.5)),
            ("f64", Value::F64(2.5)),
            ("i32", Value::I32(-7)),
            ("u32", Value::U32(9)),
            ("u64", Value::U64(1 << 40)),
            ("bool", Value::Bool(true)),
        ];
        for (name, v) in cases {
            let mut st = StateTable::new();
            let s = id(&format!("cvar:dut:{name}"));
            st.register(s.clone(), None).unwrap();
            st.set_time(1_000);
            st.record(&s, v.clone()).unwrap();
            assert_eq!(st.current_value(&s).unwrap(), Some(v.clone()));
            assert_eq!(st.changes(&s).unwrap(), vec![(1_000, v.clone())]);
            assert_eq!(st.value_at(&s, 1_000).unwrap(), Some(v));
        }
    }

    #[test]
    fn type_mismatch_on_the_generic_path_errors_and_corrupts_nothing() {
        // Rule: one signal has exactly one Value type for its lifetime. A later
        // record of a different variant is a bug — it errors with TypeMismatch and
        // leaves the column + current cache untouched (fail loud, corrupt nothing).
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&a, Value::U32(5)).unwrap();
        st.set_time(2_000);
        let err = st.record(&a, Value::F64(1.5)).unwrap_err(); // U32 column, F64 record
        assert!(matches!(
            err,
            TableError::TypeMismatch { column_kind: "U32", offending: "F64", .. }
        ));
        // The mismatched record changed nothing: still one sample, still U32.
        let log = st.changes(&a).unwrap();
        assert_eq!(log, vec![(1_000, Value::U32(5))]);
        assert_eq!(st.current_value(&a).unwrap(), Some(Value::U32(5)));

        // A subsequent well-typed record still works.
        st.set_time(3_000);
        st.record(&a, Value::U32(9)).unwrap();
        assert_eq!(st.changes(&a).unwrap().len(), 2);
    }

    #[test]
    fn every_typed_fast_lane_errors_on_a_wrong_kind_column() {
        // Each record_mirror_<t>_at rejects a column of a different established kind
        // with TypeMismatch (it defers to the generic path, which errors). Seed a
        // U32 column, then fire every non-U32 lane at it.
        let mut st = StateTable::new();
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();
        let i = st.resolve_index(&a).unwrap();
        st.set_time(1_000);
        st.record_mirror_u32_at(i, 5).unwrap(); // seeds U32

        assert!(matches!(st.record_mirror_f32_at(i, 1.0), Err(TableError::TypeMismatch { offending: "F32", .. })));
        assert!(matches!(st.record_mirror_f64_at(i, 1.0), Err(TableError::TypeMismatch { offending: "F64", .. })));
        assert!(matches!(st.record_mirror_i32_at(i, 1), Err(TableError::TypeMismatch { offending: "I32", .. })));
        assert!(matches!(st.record_mirror_u64_at(i, 1), Err(TableError::TypeMismatch { offending: "U64", .. })));
        assert!(matches!(st.record_mirror_bool_at(i, true), Err(TableError::TypeMismatch { offending: "Bool", .. })));
        // The matching lane still appends fine.
        st.set_time(2_000);
        st.record_mirror_u32_at(i, 9).unwrap();
        assert_eq!(st.changes(&a).unwrap(), vec![(1_000, Value::U32(5)), (2_000, Value::U32(9))]);
    }

    #[test]
    fn boxed_columns_are_strict_per_structured_variant() {
        // Enum and Bytes are distinct types under the one-type rule: a Boxed column
        // is seeded to one structured variant and rejects the other (and any scalar).
        let mut st = StateTable::new();
        let e = id("cvar:dut:e");
        st.register(e.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&e, Value::Enum("A".into())).unwrap(); // seeds Boxed/Enum
        st.set_time(2_000);
        st.record(&e, Value::Enum("B".into())).unwrap(); // same variant -> appends
        assert_eq!(st.changes(&e).unwrap().len(), 2);
        // Bytes into an Enum column is a mismatch.
        assert!(matches!(
            st.record(&e, Value::Bytes(vec![1])),
            Err(TableError::TypeMismatch { column_kind: "Enum", offending: "Bytes", .. })
        ));
        // A scalar into an Enum column is a mismatch too.
        assert!(matches!(
            st.record(&e, Value::U32(1)),
            Err(TableError::TypeMismatch { column_kind: "Enum", offending: "U32", .. })
        ));

        // Symmetric: a Bytes-seeded column rejects an Enum.
        let b = id("cvar:dut:b");
        st.register(b.clone(), None).unwrap();
        st.record(&b, Value::Bytes(vec![1, 2])).unwrap();
        assert!(matches!(
            st.record(&b, Value::Enum("X".into())),
            Err(TableError::TypeMismatch { column_kind: "Bytes", offending: "Enum", .. })
        ));

        // Enum/Bytes into a scalar column is also rejected.
        let s = id("cvar:dut:s");
        st.register(s.clone(), None).unwrap();
        st.record(&s, Value::U32(1)).unwrap();
        assert!(matches!(
            st.record(&s, Value::Enum("X".into())),
            Err(TableError::TypeMismatch { column_kind: "U32", offending: "Enum", .. })
        ));
    }

    #[test]
    fn typed_column_retention_evicts_and_reports_out_of_window() {
        // Retention eviction + OutOfWindow reporting + ZOH all work on a typed
        // (U32) column exactly as on the old boxed deque.
        let cfg = StateTableConfig {
            retention: Some(Duration::from_micros(2_000)),
            ..StateTableConfig::default()
        };
        let mut st = StateTable::with_config(cfg);
        let a = id("cvar:dut:a");
        st.register(a.clone(), None).unwrap();
        for (t, v) in [(1_000, 1u32), (2_000, 2), (3_000, 3), (4_000, 4)] {
            st.set_time(t);
            st.record(&a, Value::U32(v)).unwrap();
        }
        // At t=4000, cutoff=2000 → the (1000,1) sample is evicted, keeping the one
        // active at the cutoff.
        let log = st.changes(&a).unwrap();
        assert_eq!(log.len(), 3);
        assert_eq!(log[0], (2_000, Value::U32(2)));
        // A lookup before the retained window reports OutOfWindow (evicted).
        assert!(matches!(
            st.value_at(&a, 1_500),
            Err(TableError::OutOfWindow { .. })
        ));
        // ZOH within the window still holds the prior sample.
        assert_eq!(st.value_at(&a, 3_500).unwrap(), Some(Value::U32(3)));
        assert_eq!(st.value_at(&a, 4_000).unwrap(), Some(Value::U32(4)));
    }

    #[test]
    fn boxed_fallback_handles_enum_and_bytes() {
        // Enum/Bytes are structured (non-scalar) → the boxed fallback column, with
        // exact-change dedup, ZOH, and current-value all intact.
        let mut st = StateTable::new();
        let e = id("cvar:dut:e");
        st.register(e.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&e, Value::Enum("A".into())).unwrap();
        st.set_time(2_000);
        st.record(&e, Value::Enum("A".into())).unwrap(); // exact dup -> deduped
        st.set_time(3_000);
        st.record(&e, Value::Enum("B".into())).unwrap();

        let log = st.changes(&e).unwrap();
        assert_eq!(log.len(), 2);
        assert_eq!(log[0], (1_000, Value::Enum("A".into())));
        assert_eq!(log[1], (3_000, Value::Enum("B".into())));
        assert_eq!(st.current_value(&e).unwrap(), Some(Value::Enum("B".into())));
        assert_eq!(st.value_at(&e, 2_500).unwrap(), Some(Value::Enum("A".into())));

        // Bytes payloads land in the boxed fallback too.
        let b = id("cvar:dut:b");
        st.register(b.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&b, Value::Bytes(vec![1, 2, 3])).unwrap();
        assert_eq!(st.current_value(&b).unwrap(), Some(Value::Bytes(vec![1, 2, 3])));
    }

    #[test]
    fn typed_fast_lane_matches_the_generic_path() {
        // The typed mirror fast lane (record_mirror_<t>_at) must be behaviorally
        // identical to the generic record_mirror(Value::<T>) path — same dedup,
        // same historian, same ZOH — for both a float (epsilon deadband) and an
        // integer (exact) column.
        let a = id("cvar:dut:a");
        let mut fast = StateTable::new();
        let mut slow = StateTable::new();
        fast.register(a.clone(), None).unwrap();
        slow.register(a.clone(), None).unwrap();
        let idx = fast.resolve_index(&a).unwrap();
        // 1.0005 is within the 1e-3 default epsilon of 1.0 (deduped); the repeated
        // 2.0 is exact-equal (deduped) — both paths must agree.
        for (t, x) in [(1_000, 1.0f64), (2_000, 1.0005), (3_000, 2.0), (4_000, 2.0)] {
            fast.set_time(t);
            fast.record_mirror_f64_at(idx, x).unwrap();
            slow.set_time(t);
            slow.record_mirror(&a, Value::F64(x)).unwrap();
        }
        assert_eq!(fast.changes(&a), slow.changes(&a));
        assert_eq!(fast.current_value(&a).unwrap(), slow.current_value(&a).unwrap());
        for q in [500u64, 1_000, 1_500, 3_999, 5_000] {
            assert_eq!(fast.value_at(&a, q).unwrap(), slow.value_at(&a, q).unwrap());
        }

        // Integer lane (exact dedup).
        let b = id("cvar:dut:b");
        let mut fast2 = StateTable::new();
        let mut slow2 = StateTable::new();
        fast2.register(b.clone(), None).unwrap();
        slow2.register(b.clone(), None).unwrap();
        let bi = fast2.resolve_index(&b).unwrap();
        for (t, v) in [(1_000, 5u32), (2_000, 5), (3_000, 9)] {
            fast2.set_time(t);
            fast2.record_mirror_u32_at(bi, v).unwrap();
            slow2.set_time(t);
            slow2.record_mirror(&b, Value::U32(v)).unwrap();
        }
        assert_eq!(fast2.changes(&b), slow2.changes(&b));
        assert_eq!(fast2.current_value(&b).unwrap(), Some(Value::U32(9)));
    }

    // --- string-keyed scenario API ---------------------------------------

    #[test]
    fn write_read_roundtrip_on_a_registered_vsig() {
        let mut st = StateTable::new();
        let v = id("vsig:motor:angle_deg");
        st.register(v.clone(), Some("deg")).unwrap();
        st.set_time(1_000);
        st.write("vsig:motor:angle_deg", 90.0).unwrap(); // f64 literal via From
        assert_eq!(st.read("vsig:motor:angle_deg").unwrap(), Some(Value::F64(90.0)));
        // The write went through the historian, not a side channel.
        assert_eq!(st.current_value(&v).unwrap(), Some(Value::F64(90.0)));
    }

    #[test]
    fn write_to_unregistered_id_errors() {
        let mut st = StateTable::new();
        assert!(matches!(
            st.write("cvar:dut:missing", 1u32),
            Err(AccessError::Table(TableError::UnknownSignal(_)))
        ));
    }

    #[test]
    fn unparseable_or_unknown_sig_type_id_errors_not_panics() {
        let mut st = StateTable::new();
        // Too few segments and an unapproved sig_type both surface as a Parse error.
        assert!(matches!(st.write("cvar:onlytwo", 1u32), Err(AccessError::Parse(_))));
        assert!(matches!(st.write("bogus:src:name", 1u32), Err(AccessError::Parse(_))));
        assert!(matches!(st.read("bogus:src:name"), Err(AccessError::Parse(_))));
    }

    #[test]
    fn value_from_impls_cover_the_literal_shapes() {
        assert_eq!(Value::from(true), Value::Bool(true));
        assert_eq!(Value::from(9u32), Value::U32(9));
        assert_eq!(Value::from(1u64 << 40), Value::U64(1 << 40));
        assert_eq!(Value::from(90.0f64), Value::F64(90.0));
        // And a literal flows through write() to drive a signal.
        let mut st = StateTable::new();
        st.register(id("vsig:m:x"), Some("V")).unwrap();
        st.write("vsig:m:x", 1.5f64).unwrap();
        assert_eq!(st.read("vsig:m:x").unwrap(), Some(Value::F64(1.5)));
    }

    #[test]
    fn last_change_us_tracks_accepted_changes_only() {
        let mut st = StateTable::new();
        let a = id("vsig:m:a");
        st.register(a.clone(), None).unwrap();
        assert_eq!(st.last_change_us(&a).unwrap(), None); // never recorded
        st.set_time(1_000);
        st.record(&a, Value::F64(1.0)).unwrap();
        assert_eq!(st.last_change_us(&a).unwrap(), Some(1_000));
        // A same-value re-record dedups: the change time does not advance.
        st.set_time(2_000);
        st.record(&a, Value::F64(1.0)).unwrap();
        assert_eq!(st.last_change_us(&a).unwrap(), Some(1_000));
        // A real change advances it.
        st.set_time(3_000);
        st.record(&a, Value::F64(2.0)).unwrap();
        assert_eq!(st.last_change_us(&a).unwrap(), Some(3_000));
    }

    // --- unit-ask boundary (string API) ----------------------------------

    const HALF_PI: f64 = std::f64::consts::FRAC_PI_2;

    #[test]
    fn split_unit_ask_disambiguates_bracket_forms() {
        // Non-numeric bracket → unit ask, stripped off the id.
        assert_eq!(split_unit_ask("vsig:m:angle[deg]"), ("vsig:m:angle", Some("deg")));
        // All-digit bracket → cvar array index, left on the id.
        assert_eq!(split_unit_ask("cvar:d:counts[6]"), ("cvar:d:counts[6]", None));
        // No trailing bracket.
        assert_eq!(split_unit_ask("vsig:m:angle"), ("vsig:m:angle", None));
        // Empty bracket is not an ask.
        assert_eq!(split_unit_ask("vsig:m:x[]"), ("vsig:m:x[]", None));
    }

    #[test]
    fn write_with_deg_ask_stores_canonical_rad() {
        let mut st = StateTable::new();
        let a = id("vsig:motor:angle");
        st.register(a.clone(), Some("rad")).unwrap();
        st.set_time(1_000);
        st.write("vsig:motor:angle[deg]", 90.0).unwrap();
        // Stored canonical: 90 deg == π/2 rad, in an F64 column (empty seeds F64).
        match st.current_value(&a).unwrap() {
            Some(Value::F64(x)) => assert!((x - HALF_PI).abs() < 1e-9),
            other => panic!("expected F64 canonical, got {other:?}"),
        }
        assert_eq!(st.changes(&a).unwrap().len(), 1);
    }

    #[test]
    fn read_with_deg_ask_converts_canonical_to_ask() {
        let mut st = StateTable::new();
        let a = id("vsig:motor:angle");
        st.register(a.clone(), Some("rad")).unwrap();
        st.set_time(1_000);
        st.record(&a, Value::F64(HALF_PI)).unwrap();
        match st.read("vsig:motor:angle[deg]").unwrap() {
            Some(Value::F64(x)) => assert!((x - 90.0).abs() < 1e-9),
            other => panic!("expected converted F64, got {other:?}"),
        }
        // Never-recorded signal: an ask read still returns None.
        let b = id("vsig:motor:beta");
        st.register(b, Some("rad")).unwrap();
        assert_eq!(st.read("vsig:motor:beta[deg]").unwrap(), None);
    }

    #[test]
    fn bare_id_is_the_canonical_unit_untouched() {
        let mut st = StateTable::new();
        let a = id("vsig:motor:angle");
        st.register(a.clone(), Some("rad")).unwrap();
        st.set_time(1_000);
        // A bare write records verbatim (no conversion), a bare read returns it.
        st.write("vsig:motor:angle", 1.5f64).unwrap();
        assert_eq!(st.read("vsig:motor:angle").unwrap(), Some(Value::F64(1.5)));
    }

    #[test]
    fn all_digit_bracket_is_a_plain_cvar_index_not_a_unit_ask() {
        let mut st = StateTable::new();
        let c = id("cvar:dut:counts[6]");
        st.register(c.clone(), None).unwrap();
        st.set_time(1_000);
        // The `[6]` is part of the id: write/read resolve the plain signal, no unit
        // parsing (a None-unit signal would otherwise reject any ask).
        st.write("cvar:dut:counts[6]", 5u32).unwrap();
        assert_eq!(st.read("cvar:dut:counts[6]").unwrap(), Some(Value::U32(5)));
        assert_eq!(st.current_value(&c).unwrap(), Some(Value::U32(5)));
    }

    #[test]
    fn unknown_ask_unit_errors() {
        let mut st = StateTable::new();
        st.register(id("vsig:m:a"), Some("rad")).unwrap();
        assert!(matches!(
            st.write("vsig:m:a[furlong]", 1.0f64),
            Err(AccessError::UnknownUnit(u)) if u == "furlong"
        ));
    }

    #[test]
    fn typoed_unit_bracket_is_unknown_unit_not_unknown_signal() {
        // `[dg]` is a non-numeric bracket → a unit ask on a *registered* signal, so
        // the failure is UnknownUnit (the unit), never UnknownSignal (the id).
        let mut st = StateTable::new();
        st.register(id("vsig:m:a"), Some("rad")).unwrap();
        assert!(matches!(
            st.read("vsig:m:a[dg]"),
            Err(AccessError::UnknownUnit(u)) if u == "dg"
        ));
    }

    #[test]
    fn dimension_mismatch_errors() {
        let mut st = StateTable::new();
        st.add_unit("V", "voltage", 1.0, 0.0).unwrap();
        st.register(id("vsig:m:a"), Some("rad")).unwrap();
        assert!(matches!(
            st.write("vsig:m:a[V]", 1.0f64),
            Err(AccessError::DimensionMismatch { ask, canonical }) if ask == "V" && canonical == "rad"
        ));
    }

    #[test]
    fn unit_ask_on_a_unitless_signal_errors() {
        let mut st = StateTable::new();
        st.register(id("vsig:m:b"), None).unwrap();
        assert!(matches!(
            st.write("vsig:m:b[deg]", 1.0f64),
            Err(AccessError::UnitlessSignal(_))
        ));
    }

    #[test]
    fn unit_ask_write_coerces_to_an_f32_column() {
        let mut st = StateTable::new();
        let c = id("vsig:m:c");
        st.register(c.clone(), Some("rad")).unwrap();
        st.set_time(1_000);
        // Seed an F32 column (a model records canonical F32); an F64-arriving ask
        // write must coerce to F32 so the one-type rule holds.
        st.record(&c, Value::F32(0.0)).unwrap();
        st.set_time(2_000);
        st.write("vsig:m:c[deg]", 90.0).unwrap();
        match st.current_value(&c).unwrap() {
            Some(Value::F32(x)) => assert!((f64::from(x) - HALF_PI).abs() < 1e-6),
            other => panic!("expected F32 canonical, got {other:?}"),
        }
    }

    #[test]
    fn unit_ask_rejects_non_float_value_and_non_float_column() {
        let mut st = StateTable::new();
        let a = id("vsig:m:a");
        st.register(a.clone(), Some("rad")).unwrap();
        // A non-float ask value: nothing to convert.
        assert!(matches!(
            st.write("vsig:m:a[deg]", true),
            Err(AccessError::NonFloatValue { offending: "Bool", .. })
        ));
        // Seed a non-float (U32) column, then an ask write / read both reject it.
        let u = id("vsig:m:u");
        st.register(u.clone(), Some("rad")).unwrap();
        st.set_time(1_000);
        st.record(&u, Value::U32(1)).unwrap();
        assert!(matches!(
            st.write("vsig:m:u[deg]", 90.0),
            Err(AccessError::NonFloatColumn { column_kind: "U32", .. })
        ));
        assert!(matches!(
            st.read("vsig:m:u[deg]"),
            Err(AccessError::NonFloatColumn { column_kind: "U32", .. })
        ));
    }

    #[test]
    fn add_unit_rejects_digit_leading_and_conflicting_redefinition() {
        let mut st = StateTable::new();
        assert!(matches!(
            st.add_unit("2pi", "angle", 1.0, 0.0),
            Err(UnitError::DigitLeadingName(_))
        ));
        st.add_unit("mV", "voltage", 1e-3, 0.0).unwrap();
        st.add_unit("mV", "voltage", 1e-3, 0.0).unwrap(); // idempotent
        assert!(matches!(
            st.add_unit("mV", "voltage", 1.0, 0.0),
            Err(UnitError::Conflict(_))
        ));
    }
}
