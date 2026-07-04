//! The [`Model`] trait + the `vsig` backing of the State Table.
//!
//! A [`Model`] is a simulation model — a plant (motor, inverter, sensor) or peer
//! — that advances with sim time and produces a set of named signals. Those
//! signals are the State Table's **`vsig`** backing (see
//! `docs/sil/state-route-tables.md`): the first non-`cvar` backing, stored
//! Rust-side rather than in firmware memory.
//!
//! ## Symmetry with the `cvar` backing
//!
//! The State Table stays *pure data* — it does not know about models any more
//! than it knows about firmware. Instead, models are sampled into it exactly the
//! way firmware statics are:
//!
//! - **cvar:** the driver reads a firmware static via [`Backend::read_cvar`] and
//!   calls [`StateTable::record`].
//! - **vsig:** the driver reads a model signal via [`Model::read`] and calls
//!   [`StateTable::record`].
//!
//! The only asymmetry is registration: firmware entries are auto-derived from
//! DWARF, whereas a model *declares* its signals ([`Model::signals`]) which the
//! driver registers up front. [`register_model`] and [`record_model`] are the
//! thin glue that closes this loop; the future engine loop calls
//! [`Model::advance`] then [`record_model`] each tick.
//!
//! [`Backend::read_cvar`]: crate::backend::Backend::read_cvar

use crate::signal::{ParseError, SignalId, Value};
use crate::state_table::{StateTable, TableError};
use thiserror::Error;

/// One signal a [`Model`] exposes: its local name (the `<local>` segment of the
/// `vsig:<source>:<local>` id) plus optional unit metadata.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelSignal {
    pub local: String,
    pub unit: Option<String>,
}

impl ModelSignal {
    pub fn new(local: &str, unit: Option<&str>) -> Self {
        Self {
            local: local.to_string(),
            unit: unit.map(str::to_string),
        }
    }
}

/// A simulation model: it advances with sim time and produces (later also
/// consumes) a set of named signals, surfaced into the State Table as
/// `vsig:<name>:<local>` entries.
///
/// Kept deliberately minimal — identity, the signals it exposes, an advance
/// step, and a per-signal read. Real plant models (motor, inverter, sensors)
/// implement this on the instantiation side; [`RampModel`] is voyant's reference
/// impl. Impls must be **deterministic** (D7): no wall-clock, no un-seeded RNG.
pub trait Model {
    /// The model's `source` namespace — the `<source>` segment of its vsig ids
    /// (e.g. `motor`). Stable for the model's lifetime.
    fn name(&self) -> &str;

    /// The signals this model exposes. Used once, at registration, to create the
    /// model's `vsig` State Table entries.
    fn signals(&self) -> Vec<ModelSignal>;

    /// Advance the model by `dt_us` microseconds of sim time. The engine calls
    /// this once per tick before routes propagate; afterwards [`read`](Self::read)
    /// reflects the new state. A model owns its own time integration.
    fn advance(&mut self, dt_us: u64);

    /// Read the current value of one signal by local name; `None` if `local` is
    /// not one of this model's signals.
    fn read(&self, local: &str) -> Option<Value>;
}

/// The canonical `vsig:<model_name>:<local>` id for one of a model's signals.
pub fn vsig_id(model_name: &str, local: &str) -> Result<SignalId, ParseError> {
    SignalId::new("vsig", model_name, local, None)
}

/// Errors from the model↔State-Table glue.
#[derive(Debug, Clone, PartialEq, Error)]
pub enum ModelError {
    #[error("model signal id: {0}")]
    Id(#[from] ParseError),
    #[error(transparent)]
    Table(#[from] TableError),
}

/// Register all of a model's signals as `vsig` entries in the State Table (the
/// vsig analog of DWARF auto-derivation for cvars — done once, up front).
pub fn register_model(st: &mut StateTable, model: &dyn Model) -> Result<(), ModelError> {
    for sig in model.signals() {
        let id = vsig_id(model.name(), &sig.local)?;
        st.register(id, sig.unit.as_deref())?;
    }
    Ok(())
}

/// Sample every signal of a model and [`record`](StateTable::record) it into the
/// State Table at the table's current time — the vsig analog of cvar sampling.
/// Call after [`Model::advance`] and [`StateTable::set_time`].
pub fn record_model(st: &mut StateTable, model: &dyn Model) -> Result<(), ModelError> {
    for sig in model.signals() {
        if let Some(v) = model.read(&sig.local) {
            let id = vsig_id(model.name(), &sig.local)?;
            st.record(&id, v)?;
        }
    }
    Ok(())
}

/// A reference [`Model`]: a linear ramp source, for exercising the `vsig`
/// backing in tests and demos. Exposes one signal, `value`, that advances at a
/// fixed slope per second of sim time. Deterministic, no dependencies — real
/// plant models live on the instantiation side.
pub struct RampModel {
    name: String,
    unit: Option<String>,
    slope_per_s: f64,
    elapsed_us: u64,
    value: f64,
}

impl RampModel {
    /// A ramp named `name` whose `value` grows by `slope_per_s` units every
    /// second of sim time, starting at 0.
    pub fn new(name: &str, slope_per_s: f64, unit: Option<&str>) -> Self {
        Self {
            name: name.to_string(),
            unit: unit.map(str::to_string),
            slope_per_s,
            elapsed_us: 0,
            value: 0.0,
        }
    }
}

impl Model for RampModel {
    fn name(&self) -> &str {
        &self.name
    }

    fn signals(&self) -> Vec<ModelSignal> {
        vec![ModelSignal::new("value", self.unit.as_deref())]
    }

    fn advance(&mut self, dt_us: u64) {
        self.elapsed_us += dt_us;
        self.value = self.slope_per_s * (self.elapsed_us as f64) / 1e6;
    }

    fn read(&self, local: &str) -> Option<Value> {
        match local {
            "value" => Some(Value::F64(self.value)),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vsig_id_is_canonical() {
        let id = vsig_id("motor", "angle_rad").unwrap();
        assert_eq!(id.as_str(), "vsig:motor:angle_rad");
        assert_eq!(id.sig_type(), "vsig");
        assert_eq!(id.source(), "motor");
        assert_eq!(id.name(), "angle_rad");
    }

    #[test]
    fn ramp_advances_with_time() {
        // 1000 units/s => +1.0 per 1 ms tick.
        let mut m = RampModel::new("ramp", 1000.0, Some("counts"));
        assert_eq!(m.read("value"), Some(Value::F64(0.0)));
        m.advance(1_000);
        assert_eq!(m.read("value"), Some(Value::F64(1.0)));
        m.advance(1_000);
        assert_eq!(m.read("value"), Some(Value::F64(2.0)));
        assert_eq!(m.read("nonexistent"), None);
    }

    #[test]
    fn registers_vsig_entries() {
        let mut st = StateTable::new();
        let m = RampModel::new("ramp", 1000.0, Some("counts"));
        register_model(&mut st, &m).unwrap();
        assert_eq!(st.len(), 1);
        let id = vsig_id("ramp", "value").unwrap();
        // Registered but not yet recorded.
        assert_eq!(st.current_value(&id).unwrap(), None);
    }

    #[test]
    fn records_model_into_historian_with_time() {
        let mut st = StateTable::new();
        let mut m = RampModel::new("ramp", 1000.0, None);
        register_model(&mut st, &m).unwrap();
        let id = vsig_id("ramp", "value").unwrap();

        for tick in 1..=4u64 {
            m.advance(1_000);
            st.set_time(tick * 1_000);
            record_model(&mut st, &m).unwrap();
        }
        // value 1,2,3,4 all beyond epsilon -> four change-log entries.
        assert_eq!(st.changes(&id).unwrap().len(), 4);
        assert_eq!(st.current_value(&id).unwrap(), Some(&Value::F64(4.0)));
        // ZOH lookup between records holds the prior sample.
        assert_eq!(st.value_at(&id, 2_500).unwrap(), Some(&Value::F64(2.0)));
    }

    #[test]
    fn model_usable_behind_dyn() {
        // The engine loop drives models as `&mut dyn Model`; prove object-safety.
        let mut m = RampModel::new("ramp", 1.0, None);
        let dm: &mut dyn Model = &mut m;
        dm.advance(1_000_000); // 1 s -> value 1.0
        assert_eq!(dm.name(), "ramp");
        assert_eq!(dm.read("value"), Some(Value::F64(1.0)));
    }

    #[test]
    fn register_rejects_duplicate_signal() {
        let mut st = StateTable::new();
        let m = RampModel::new("ramp", 1.0, None);
        register_model(&mut st, &m).unwrap();
        assert!(matches!(
            register_model(&mut st, &m),
            Err(ModelError::Table(TableError::DuplicateSignal(_)))
        ));
    }
}
