//! Reference models shared by the tests and the perf bin.

use voyant::{vsig_id, LogLevel, Member, MemberCtx, SignalId, StateTable, Value};

/// A minimal integer "sensor" model: one `counts` signal stepping a fixed amount
/// each tick. Emits [`Value::U32`] so a route drives a firmware `uint32_t` with no
/// conversion (routes are pure copies; float→counts is a sensor model's job).
pub struct CountsRampModel {
    name: String,
    step: u32,
    counts: u32,
}

impl CountsRampModel {
    pub fn new(name: &str, step: u32) -> Self {
        Self {
            name: name.to_string(),
            step,
            counts: 0,
        }
    }

    fn counts_id(&self) -> SignalId {
        vsig_id(&self.name, "counts").expect("valid vsig id")
    }
}

impl Member for CountsRampModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        self.counts = self.counts.wrapping_add(self.step);
        let id = self.counts_id();
        if let Err(e) = ctx.st.record(&id, Value::U32(self.counts)) {
            ctx.st.log(
                LogLevel::Warning,
                &self.name,
                format!("record {id} failed: {e}"),
            );
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.counts_id(), Some("counts"));
        }
    }
}
