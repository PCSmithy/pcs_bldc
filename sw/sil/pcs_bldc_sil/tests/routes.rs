//! The Route Table on the real production path: a model's `vsig` drives a firmware
//! `cvar` via the FirmwareMember flush (with suspend/resume), and a genuine
//! two-member feedback loop needs the delayed (ZOH-cut) backward edge.

use pcs_bldc_sil::{cvar, CountsRampModel, Sil, SOURCE};
use voyant::{
    vsig_id, EngineError, LogLevel, Member, MemberCtx, RouteError, SignalId, StateTable, Value,
};

#[test]
fn route_drives_firmware_cvar_from_model() {
    const STEP: u32 = 25; // stays within a u8 destination byte
    let src = vsig_id("sensor", "counts").expect("valid vsig id");
    // The sim USB rx byte: read by the firmware, never written by it — so a routed
    // value survives the tick and can be asserted after.
    let dst = cvar("HW_USB_sim_data.rx[0]");

    let mut sim = Sil::new();
    sim.add_member(CountsRampModel::new("sensor", STEP));
    let mut fwm = sim.load_firmware(SOURCE);
    // rx is a 512-byte buffer, over the array threshold, so the driven element is
    // registered explicitly. A registered cvar the framework command-writes (the
    // route) is flushed into memory by the member automatically (fresh-dirty flush).
    fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
    sim.add_member(fwm);
    sim.add_route(src.clone(), dst.clone()).expect("add route");

    // Active route: firmware memory tracks the model exactly, step by step. Direct
    // reads assert the routed value reached firmware MEMORY (the member's flush).
    let mut tracked = true;
    let mut last = 0u64;
    for tick in 1..=4u64 {
        sim.step().expect("engine step");
        let got = sim.fw().read_cvar(dst.name()).as_u64().unwrap_or(0);
        tracked &= got == (tick * STEP as u64);
        last = got;
    }
    assert!(
        tracked && (last == 4 * STEP as u64),
        "route drives a firmware cvar from a model vsig: rx[0] tracked to {last} over 4 steps (expect {})",
        4 * STEP
    );

    // Suspend: the model keeps advancing, but the route stops recording the dest
    // entry, so the firmware member re-flushes the held value — firmware memory must
    // NOT follow the model.
    sim.suspend_route(&src, &dst).expect("suspend");
    sim.step().expect("engine step");
    let held = sim.fw().read_cvar(dst.name()).as_u64().unwrap_or(0);
    let model_now = sim
        .read(src.as_str())
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    assert!(
        (held == last) && (model_now > held),
        "suspended route stops driving: rx[0] held at {held} while model advanced to {model_now}"
    );

    // Resume: the destination jumps to the model's current value again.
    sim.resume_route(&src, &dst).expect("resume");
    sim.step().expect("engine step");
    let resumed = sim.fw().read_cvar(dst.name()).as_u64().unwrap_or(0);
    let model_after = sim
        .read(src.as_str())
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    assert!(
        resumed == model_after,
        "resumed route drives the destination again: rx[0] = {resumed} (expect current model {model_after})"
    );
}

/// A model for the feedback loop: reads input `in` (a firmware counter, delivered on
/// the *delayed* backward edge) and emits `out = in % 200` (a [`Value::U32`] within a
/// `u8` so it can drive the sim USB `rx[0]` byte with no conversion).
struct LoopModel {
    name: String,
    out: u32,
}

impl LoopModel {
    fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            out: 0,
        }
    }
    fn in_id(&self) -> SignalId {
        vsig_id(&self.name, "in").expect("valid vsig id")
    }
    fn out_id(&self) -> SignalId {
        vsig_id(&self.name, "out").expect("valid vsig id")
    }
}

impl Member for LoopModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        let input = ctx
            .st
            .current_value(&self.in_id())
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_u64)
            .unwrap_or(0);
        self.out = (input % 200) as u32;
        let id = self.out_id();
        if let Err(e) = ctx.st.record(&id, Value::U32(self.out)) {
            ctx.st.log(
                LogLevel::Warning,
                &self.name,
                format!("record {id} failed: {e}"),
            );
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.in_id(), None);
            let _ = st.register(self.out_id(), Some("counts"));
        }
    }
}

#[test]
fn feedback_loop() {
    // Forward (zero-latency): the model's `out` drives a firmware sim-input byte.
    // Backward (DELAYED, the ZOH cut): the firmware counter `task1msRuns` -> model
    // `in`. With member order [model, firmware] the backward edge is backward in
    // registration order: the validator rejects it while zero-latency, accepts it
    // once delayed. We catch the step error, rewire live, then assert the sequence.
    let out = vsig_id("loop_model", "out").expect("valid vsig id");
    let inp = vsig_id("loop_model", "in").expect("valid vsig id");
    let counter = cvar("task1msRuns"); // firmware output (sampled)
    let rx = cvar("HW_USB_sim_data.rx[0]"); // firmware input (driven)

    let mut sim = Sil::new();
    sim.add_member(LoopModel::new("loop_model")); // idx 0
    let mut fwm = sim.load_firmware(SOURCE); // idx 1
    fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
    sim.add_member(fwm);

    // Forward edge: model out -> firmware rx byte (zero-latency, model before fw).
    sim.add_route(out.clone(), rx.clone())
        .expect("add forward route");
    // Backward edge as ZERO-latency first: firmware counter -> model in. This is a
    // backward edge (source firmware is registered AFTER the consuming model), so the
    // validator must reject it at the next step.
    sim.add_route(counter.clone(), inp.clone())
        .expect("add backward route");

    let rejected = matches!(
        sim.step(),
        Err(EngineError::Route(RouteError::BackwardRoute { .. }))
    );
    assert!(
        rejected,
        "validator rejects the zero-latency feedback loop until the backward edge is declared delayed"
    );

    // Fix the wiring LIVE: drop the zero-latency backward edge, re-add it delayed (the
    // explicit ZOH sample/actuation cut). Rewire-at-runtime is legal.
    sim.remove_route(&counter, &inp)
        .expect("remove backward route");
    sim.add_delayed_route(counter.clone(), inp.clone())
        .expect("add delayed backward route");

    // Predicted deterministic sequence for rx[0] read after each step:
    //   step 1: model in is unset (fw counter not yet sampled by this engine) -> 0.
    //   step k>=2: in = task1msRuns as of the end of step k-1 = base + (k-1),
    //             so rx[0] = (base + (k-1)) % 200.
    // `base` is the firmware counter right before the first successful step (the
    // rejected step returned early and did NOT advance the firmware).
    let base = sim.fw().read_cvar(counter.name()).as_u64().unwrap_or(0);
    const N: u64 = 6;
    let got: Vec<u64> = (1..=N)
        .map(|_| {
            sim.step().expect("engine step");
            sim.fw().read_cvar(rx.name()).as_u64().unwrap_or(0)
        })
        .collect();
    let predicted: Vec<u64> = (1..=N)
        .map(|k| if k == 1 { 0 } else { (base + (k - 1)) % 200 })
        .collect();

    assert_eq!(
        got, predicted,
        "delayed feedback loop produces the exact predicted sequence (base counter {base})"
    );
}
