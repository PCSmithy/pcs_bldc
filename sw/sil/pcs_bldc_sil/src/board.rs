//! The pcs_bldc board world — one firmware instance wired to the plant, both AS5048s
//! and the current-sense front end, every route live. Build one with [`board`] and
//! suspend the routes a scenario wants to drive by hand.
//!
//! The constants here mirror firmware `app_*` macros and board wiring; the read
//! helpers name the statics more than one check reads back.

use crate::{
    cid, vid, wire_bridge, wire_current_sense, wiring::BridgeRoutes, wiring::CurrentSenseRoutes,
    As5048Model, CurrentSenseModel, MotorModel, Sil, SOURCE,
};
use voyant::vsig_id;

/// Member names: the plant, its commutation encoder, the user dial, the sense front end.
pub const MOTOR: &str = "motor";
pub const ENCODER: &str = "as5048_motor";
pub const DIAL: &str = "dial";
pub const SENSE: &str = "current_sense";

/// AS5048 resolution — 14 bits over one revolution.
pub const COUNTS_PER_REV: f64 = 16384.0;

/// Bench-measured encoder noise (sigma in LSB) and the seed each instance runs with.
pub const ENCODER_NOISE_LSB: f32 = 1.52;
pub const ENCODER_SEED: u64 = 0;
pub const DIAL_NOISE_LSB: f32 = 1.22;
pub const DIAL_SEED: u64 = 1;

/// The USB-PD bus voltage the board runs on.
pub const VBUS_V: f64 = 24.0;

/// The gate driver's 200 ms configure + status pass, plus slack.
pub const GATE_BRINGUP_MS: u64 = 300;

/// `ALIGNMENT_DUTY_CYCLE` / `ALIGNMENT_DWELL_TIMER_MS` in `app_motorControl.c`.
pub const ALIGN_DUTY: f64 = 0.1;
pub const ALIGN_DWELL_MS: u64 = 500;

/// `OVERCURRENT_PHASE_TRIP_A` / `OVERCURRENT_BUS_TRIP_A` in `app_motorControl.c`.
pub const PHASE_TRIP_A: f64 = 2.0;
pub const BUS_TRIP_A: f64 = 1.5;

/// Button PB10, active LOW (idle HIGH via pull-up). Port B is enum index 1, bit 10.
pub const INPUT_LEVEL_PB10: &str = "HW_GPIO_data.inputLevel[1][10]";
pub const GPIO_LEVEL_LOW: u32 = 0;
pub const GPIO_LEVEL_HIGH: u32 = 1;

/// Gate-driver STATUS (reg 0x80 = 128) on the sim I2C register file: BUS_2 (index 1),
/// sole device on the bus. LOCK is bit 7; every fault bit clear.
pub const I2C_STATUS_REG: &str = "HW_I2C_data.buses[1].devices[0].regMem[128]";
pub const GATEDRIVER_STATUS_LOCKED: u32 = 0x80;

/// A built board world: the simulation plus the two wiring bundles, each bundle the
/// fault-injection seam for its edges.
pub struct Board {
    pub sim: Sil,
    pub bridge: BridgeRoutes,
    pub sense: CurrentSenseRoutes,
}

/// Build the board world with the shaft starting at `initial_angle_rad`: the bus
/// energized, the gate driver seeded operational, the button idle, and both wiring
/// bundles live. The encoders carry their measured noise (the quantized
/// `raw_encoder_ticks` output is noise-free; only the wire frames are perturbed).
pub fn board(initial_angle_rad: f64) -> Board {
    let mut sim = Sil::new();

    // Producer → sensor → firmware, so the zero-latency routes land the same tick.
    sim.add_member(MotorModel::new(MOTOR, initial_angle_rad));
    let encoder =
        sim.add_member(As5048Model::new(ENCODER, 0.0).with_noise(ENCODER_NOISE_LSB, ENCODER_SEED));
    let dial = sim.add_member(As5048Model::new(DIAL, 0.0).with_noise(DIAL_NOISE_LSB, DIAL_SEED));
    sim.add_member(CurrentSenseModel::new(SENSE));

    // Force-registered before the member enumerates its mirror: a multi-dim array
    // element and an over-threshold one, both written by the checks.
    let mut fwm = sim.load_firmware(SOURCE);
    fwm.register_cvar_in_state_table(INPUT_LEVEL_PB10);
    fwm.register_cvar_in_state_table(I2C_STATUS_REG);
    sim.add_member(fwm);

    // The plant's shaft angle is the encoder's input — no check ever writes it.
    sim.add_route(
        vsig_id(MOTOR, "angle").expect("valid vsig id"),
        vsig_id(ENCODER, "angle").expect("valid vsig id"),
    )
    .expect("route the shaft angle into the commutation encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_1", encoder)
        .expect("link the commutation encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_2", dial)
        .expect("link the dial encoder");
    let bridge = wire_bridge(&mut sim, SOURCE, MOTOR).expect("wire the bridge into the plant");
    let sense = wire_current_sense(&mut sim, MOTOR, SENSE, SOURCE).expect("wire the sense chain");

    sim.write(&vid(MOTOR, "v_bus"), VBUS_V)
        .expect("energize the bus");
    sim.write(&cid(I2C_STATUS_REG), GATEDRIVER_STATUS_LOCKED)
        .expect("seed gate STATUS");
    // The sim's inputLevel defaults LOW, which — active-low — reads as pressed.
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH)
        .expect("button idle high");

    Board { sim, bridge, sense }
}

/// One firmware observation port (`vsig:pcs_bldc:<name>`) as an `f64`.
pub fn port(sim: &Sil, name: &str) -> f64 {
    sim.read_f64(&vid(SOURCE, name))
}

/// Channel-0 motor control has latched a fault.
pub fn fault_latched(sim: &Sil) -> bool {
    sim.read_bool(&cid("app_motorControl_data.channels[0].faultLatched"))
}

/// One phase current in amps as the firmware itself decoded it on its last 1 ms pass —
/// the executed decode, not a re-computation.
pub fn decoded_phase_a(sim: &Sil, phase: usize) -> f64 {
    sim.read_f64(&cid(&format!(
        "app_motorControl_data.channels[0].phaseCurrent_a[{phase}]"
    )))
}

/// The bus current in amps as the firmware itself decoded it.
pub fn decoded_bus_a(sim: &Sil) -> f64 {
    sim.read_f64(&cid("app_motorControl_data.channels[0].busCurrent"))
}

/// Channel-0 gate driver reads operational (mirrors `dev_gateDriver_isOperational`).
pub fn gate_operational(sim: &Sil) -> bool {
    let c = |f: &str| sim.read_bool(&cid(&format!("dev_gateDriver_data.channels[0].{f}")));
    c("configured")
        && c("statusOk")
        && c("locked")
        && !c("resetLatched")
        && !c("vdsProtection")
        && !c("thermalShutdown")
        && !c("vccUndervoltage")
}
