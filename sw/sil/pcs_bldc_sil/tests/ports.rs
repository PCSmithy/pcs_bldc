//! The port seam end to end: sim ADC input ports carry a model's volts (converted to
//! counts by the driver), and sim TIM output ports publish the commanded bridge
//! state (dark at boot).

use pcs_bldc_sil::{cid, vid, Sil, BRIDGE_PORTS, SOURCE};
use voyant::{vsig_id, LogLevel, Member, MemberCtx, SignalId, StateTable, Value};

/// A model holding one output `volts` constant — the simplest plant/sensor stand-in
/// driving an analog pin.
struct VoltsModel {
    name: String,
    volts: f64,
}

impl VoltsModel {
    fn new(name: &str, volts: f64) -> Self {
        Self {
            name: name.to_string(),
            volts,
        }
    }
    fn volts_id(&self) -> SignalId {
        vsig_id(&self.name, "volts").expect("valid vsig id")
    }
}

impl Member for VoltsModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        let id = self.volts_id();
        if let Err(e) = ctx.st.record(&id, Value::F64(self.volts)) {
            ctx.st.log(
                LogLevel::Warning,
                &self.name,
                format!("record {id} failed: {e}"),
            );
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.volts_id(), Some("V"));
        }
    }
}

// [test->fw~hal_adc_004~1]
#[test]
fn adc_ports() {
    const VOLTS: f64 = 1.234;
    // ADC1 regular input 6 (port ADC1_IN6) is driven; input 1 stays undriven.
    let port = SignalId::new("vsig", SOURCE, "ADC1_IN6", None).expect("valid port id");
    let driven_counts = "HW_ADC_data.channelData[0].counts[6]";
    let neighbor_counts = "HW_ADC_data.channelData[0].counts[1]";

    let mut sim = Sil::new();
    sim.add_member(VoltsModel::new("pin_model", VOLTS));
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    // The sim drivers register their ports during sil_fw_start; the FirmwareMember
    // applied them to the table at add_member. The board config enables 10 regular
    // inputs (5 per ADC), so 10 ADC-named input ports exist under this member's name.
    let n_ports = sim
        .state()
        .signals()
        .filter(|s| {
            (s.sig_type() == "vsig") && (s.source() == SOURCE) && s.name().starts_with("ADC")
        })
        .count();
    assert_eq!(
        n_ports, 10,
        "sim ADC registered one input port per enabled input"
    );

    // Route the model's volts into the port (native format: volts -> volts).
    sim.add_route(
        vsig_id("pin_model", "volts").expect("valid vsig id"),
        port.clone(),
    )
    .expect("add route");

    // Expected counts: mirror of the sim driver's volts->counts math, with
    // numBits/vref read via DWARF from the sim channel config. Static config, read
    // before the step loop while the mirror is still cold.
    let vref = sim
        .fw()
        .read_cvar("HW_ADC_channelConfig[0].vref")
        .as_f64()
        .unwrap_or(0.0);
    let num_bits = sim
        .fw()
        .read_cvar("HW_ADC_channelConfig[0].numBits")
        .as_u64()
        .unwrap_or(0);
    let max_counts = (1u64 << num_bits) - 1;
    let scaled = ((VOLTS / vref) * (max_counts as f64)) + 0.5;
    let expected = if scaled >= (max_counts as f64) {
        max_counts
    } else {
        scaled as u64
    };

    // Step a few ticks; sample both counts statics after each. The driven input must
    // sit at the exact quantized value once the port takes effect; the undriven
    // neighbor must keep ramping.
    let mut driven: Vec<u64> = Vec::new();
    let mut neighbor: Vec<u64> = Vec::new();
    for _ in 0..6 {
        sim.step().expect("engine step");
        driven.push(sim.read_u64(&cid(driven_counts)));
        neighbor.push(sim.read_u64(&cid(neighbor_counts)));
    }
    let settled = &driven[2..];
    assert!(
        settled.iter().all(|c| *c == expected),
        "driven port converts volts -> counts: counts[6] = {settled:?} (expect {expected} = {VOLTS} V @ {num_bits} bits / {vref} V vref)"
    );
    let neighbor_settled = &neighbor[2..];
    let ramping = neighbor_settled.windows(2).any(|w| w[0] != w[1]);
    assert!(
        ramping,
        "neighboring undriven input still ramps: counts[1] = {neighbor_settled:?} (must keep changing)"
    );
}

// [test->fw~io_bridge_003~1]
#[test]
fn pwm_ports() {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    // Registered during sil_fw_start, applied to the table at add_member.
    let missing: Vec<String> = BRIDGE_PORTS
        .iter()
        .map(|(p, _)| vid(SOURCE, p))
        .filter(|id| !sim.state().signals().any(|s| s.as_str() == id))
        .collect();
    assert!(
        missing.is_empty(),
        "sim HW_TIM registers 7 PWM/bridge ports; missing: {missing:?}"
    );

    // app_motorControl re-commands the dark bridge every tick, so the ports publish 0
    // through the production setters — read them back after a few steps.
    for _ in 0..5 {
        sim.step().expect("engine step");
    }
    let wrong: Vec<String> = BRIDGE_PORTS
        .iter()
        .filter_map(|(p, _)| {
            let v = sim.read_f64(&vid(SOURCE, p));
            (v != 0.0).then(|| format!("{p}={v}"))
        })
        .collect();
    assert!(
        wrong.is_empty(),
        "PWM/bridge ports read the dark-bridge boot state (duty/enable/MOE = 0); non-zero: {wrong:?}"
    );
}
