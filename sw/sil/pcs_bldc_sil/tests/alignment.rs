//! Button-to-alignment sanity path, end to end through the State Table. Seed the gate
//! driver's I2C STATUS (LOCK set, faults clear), hold the four current-sense ADC ports
//! at zero current, tap the user button (via the GPIO input-level static) with the dial
//! sitting at zero — and the firmware's six-step drive enters its 500 ms alignment dwell:
//! U at ~0.1 duty with U+V enabled and MOE on, then `isAligned` latches with the offset
//! matching the encoder's shaft angle, no fault raised. Arming, not the dial, engages the
//! drive: at zero demand the phases idle at zero duty while the master output enable holds
//! asserted; the first dial turn commutates at once on the stored offset — no second dwell.
//!
//! Everything is injected through `sim.write(...)`; the `_sim_*` C shims are unused.
//! The button seam is `HW_GPIO_data.inputLevel[port][bit]` — a 2-D enum array. The
//! `cachedInput` mirror is recomputed from `inputLevel` at the top of every tick
//! (before `dev_switch` reads it), so it cannot hold an injected value; `inputLevel`
//! is the real seam. It is force-registered with `register_cvar_in_state_table` and
//! reached by the resolver's (newly) multi-index-aware DWARF addressing.
//! TODO - model the button press through a button model or direct ADC vsig.

use pcs_bldc_sil::{cid, As5048Model, Sil, SOURCE};
use voyant::Value;

// Button: PB10, active LOW (idle HIGH via pull-up). Port B is enum index 1, bit 10.
const INPUT_LEVEL_PB10: &str = "HW_GPIO_data.inputLevel[1][10]";
const GPIO_LEVEL_LOW: u32 = 0; // HW_GPIO_LEVEL_LOW
const GPIO_LEVEL_HIGH: u32 = 1; // HW_GPIO_LEVEL_HIGH

// Gate-driver STATUS (reg 0x80 = 128) on the sim I2C register file: BUS_2 (index 1),
// sole device on the bus → slot 0. LOCK is bit 7; every fault bit clear.
const I2C_STATUS_REG: &str = "HW_I2C_data.buses[1].devices[0].regMem[128]";
const GATEDRIVER_STATUS_LOCKED: u32 = 0x80;

fn read_bool(sim: &Sil, path: &str) -> bool {
    matches!(sim.read(&cid(path)).ok().flatten(), Some(Value::Bool(true)))
}

fn read_f64(sim: &Sil, path: &str) -> f64 {
    sim.read(&cid(path)).ok().flatten().as_ref().and_then(Value::as_f64).unwrap_or(f64::NAN)
}

fn read_u64(sim: &Sil, path: &str) -> u64 {
    sim.read(&cid(path)).ok().flatten().as_ref().and_then(Value::as_u64).unwrap_or(0)
}

fn port(sim: &Sil, name: &str) -> f64 {
    sim.read(&format!("vsig:{SOURCE}:{name}"))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or(f64::NAN)
}

/// Largest duty across the three phases — the drive magnitude the bridge is carrying.
fn max_phase_duty(sim: &Sil) -> f64 {
    ["PWM_U_duty", "PWM_V_duty", "PWM_W_duty"]
        .iter()
        .map(|p| port(sim, p))
        .fold(0.0_f64, f64::max)
}

/// The channel-0 gate driver reads operational: configured, last STATUS read good,
/// locked, every fault flag clear (mirrors `dev_gateDriver_isOperational`).
fn gate_operational(sim: &Sil) -> bool {
    let c = |f: &str| read_bool(sim, &format!("dev_gateDriver_data.channels[0].{f}"));
    c("configured")
        && c("statusOk")
        && c("locked")
        && !c("resetLatched")
        && !c("vdsProtection")
        && !c("thermalShutdown")
        && !c("vccUndervoltage")
}

/// `modeCurrent` is SIX_STEP_TRAP (enum value 1). The mirror reports it as the
/// enumerator name when DWARF names it, or `<1>` otherwise — accept both.
fn mode_is_six_step(sim: &Sil) -> bool {
    match sim.read(&cid("app_motorControl_data.channels[0].modeCurrent")).ok().flatten() {
        Some(Value::Enum(s)) => s.contains("SIX_STEP") || s == "<1>",
        other => panic!("modeCurrent mirrored as {other:?}, not an enum"),
    }
}

#[test]
fn button_tap_triggers_alignment() {
    const MOTOR_ANGLE_RAD: f32 = std::f32::consts::FRAC_PI_2; // 90 deg
    const ALIGN_DUTY: f64 = 0.1; // ALIGNMENT_DUTY_CYCLE

    let mut sim = Sil::new();

    // Models before firmware: the motor encoder answers the firmware's real SPI polls
    // (nonzero shaft angle), the dial idles at 0 until turned.
    let motor = sim.add_member(As5048Model::new("as5048_motor", MOTOR_ANGLE_RAD));
    let dial = sim.add_member(As5048Model::new("dial", 0.0));

    // Force-register the two over-threshold / multi-dim statics this scenario writes,
    // before the member enumerates its mirror.
    let mut fwm = sim.load_firmware(SOURCE);
    fwm.register_cvar_in_state_table(INPUT_LEVEL_PB10);
    fwm.register_cvar_in_state_table(I2C_STATUS_REG);
    sim.add_member(fwm);

    sim.link_duplex("spi:pcs_bldc:AS5048_1", motor).expect("link motor encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_2", dial).expect("link dial encoder");

    // --- Defuse the bring-up traps (all through the table) --------------------------

    // Current-sense: hold each phase shunt at its 1.65 V zero-current midpoint and the
    // bus shunt at 0 V (ground-referenced). Ports are ZOH — one write holds. Mapping +
    // scalings per IO_bridge_channels.c: U=ADC1_IN6, V=ADC2_IN7, W=ADC1_IN8, bus=ADC2_IN11.
    sim.write("vsig:pcs_bldc:ADC1_IN6", 1.65).expect("phase U zero current");
    sim.write("vsig:pcs_bldc:ADC2_IN7", 1.65).expect("phase V zero current");
    sim.write("vsig:pcs_bldc:ADC1_IN8", 1.65).expect("phase W zero current");
    sim.write("vsig:pcs_bldc:ADC2_IN11", 0.0).expect("bus zero current");

    // Gate driver: seed STATUS with LOCK set + faults clear so the 200 ms configure +
    // status pass reads operational.
    // TODO - set through i2c sig_type (similar to spi).
    sim.write(&cid(I2C_STATUS_REG), GATEDRIVER_STATUS_LOCKED).expect("seed gate STATUS");

    // Button idle: drive PB10 HIGH (released). The sim's inputLevel defaults LOW, which
    // — active-low — would read as pressed; HIGH gives a clean released baseline.
    // TODO - set through GPIO vsig.
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button idle high");

    // --- Let the button settle + the gate driver come up ----------------------------
    sim.run_for_ms(300);
    assert!(gate_operational(&sim), "gate driver operational after a 200 ms configure+status pass");
    assert!(!read_bool(&sim, "app_motorControl_data.channels[0].faultLatched"), "no fault before drive");
    // The bridge is dark before any drive command: U duty sits at 0.
    let duty_dark = port(&sim, "PWM_U_duty");
    assert_eq!(duty_dark, 0.0, "bridge dark (U duty 0) before the drive command");

    // --- Button tap: press then release inside the fault-clear hold window ----------
    // Press (LOW), held past the 20 ms debounce so dev_switch latches ACTIVE.
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW).expect("button press");
    sim.run_for_ms(30);
    // Release (HIGH), held past debounce so dev_switch latches INACTIVE → tap.
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button release");
    sim.run_for_ms(30);

    // The lone tap becomes a run/stop toggle once the 300 ms double-tap window elapses.
    // Arming alone engages the drive: the dial is left at zero, yet the mode goes
    // SIX_STEP and alignment begins.
    sim.run_for_ms(320);

    // --- Assert the alignment dwell drives the documented pattern -------------------
    assert!(mode_is_six_step(&sim), "arming (not the dial) put motor control into SIX_STEP");
    assert!(
        !read_bool(&sim, "app_motorControl_data.channels[0].isAligned"),
        "still aligning during the dwell"
    );
    let duty_u = port(&sim, "PWM_U_duty");
    assert!((duty_u - ALIGN_DUTY).abs() < 0.02, "alignment holds U at ~0.1 duty, got {duty_u}");
    assert_eq!(port(&sim, "PWM_U_enabled"), 1.0, "U enabled during alignment");
    assert_eq!(port(&sim, "PWM_V_enabled"), 1.0, "V enabled during alignment");
    assert_eq!(port(&sim, "PWM_W_enabled"), 0.0, "W held off during alignment");
    assert_eq!(port(&sim, "TIM1_MOE"), 1.0, "master output enable on during alignment");

    // --- Ride out the 500 ms dwell; capture the U-duty trace ------------------------
    // TODO - refactor this once the Expectation Engine exists
    // (`with sim.expect().reaches('.isAligned', true): sim.run_for(500)`).
    let mut duty_trace: Vec<f64> = vec![duty_u];
    let mut aligned_at_ms: Option<u64> = None;
    for ms in 1..=520u64 {
        sim.run_for_ms(1);
        duty_trace.push(port(&sim, "PWM_U_duty"));
        if aligned_at_ms.is_none()
            && read_bool(&sim, "app_motorControl_data.channels[0].isAligned")
        {
            aligned_at_ms = Some(ms);
        }
    }
    assert!(aligned_at_ms.is_some(), "isAligned latched within the dwell window");

    // --- The captured offset equals the encoder's decoded shaft angle ---------------
    // Derive the expected rad from what we commanded plus the firmware's own reverse
    // config (the decode contract, not a frozen convention).
    let reverse = matches!(
        sim.fw().read_cvar("IO_AS5048_channelConfig[0].reverse"),
        Value::Bool(true)
    );
    let wire_raw = (f64::from(MOTOR_ANGLE_RAD) * 16384.0 / (2.0 * std::f64::consts::PI)).round();
    let decoded_raw = if reverse { 16384.0 - wire_raw } else { wire_raw };
    let expected_offset_rad = decoded_raw * 2.0 * std::f64::consts::PI / 16384.0;

    let offset = read_f64(&sim, "app_motorControl_data.channels[0].alignmentOffset_rad");
    let decoded_rad = read_f64(&sim, "IO_AS5048_data.channels[0].angle_rad");
    assert!(
        (offset - expected_offset_rad).abs() < 0.01,
        "alignmentOffset {offset} matches the commanded shaft angle {expected_offset_rad} (reverse={reverse})"
    );
    assert!(
        (offset - decoded_rad).abs() < 0.01,
        "alignmentOffset {offset} equals the encoder's decoded angle_rad {decoded_rad}"
    );

    // --- Zero demand idles the phases without dropping the bridge --------------------
    // Aligned, armed, dial at zero: the phases carry zero duty while the master output
    // enable holds asserted across the whole window — the bridge does not flap.
    assert!(max_phase_duty(&sim) < 0.02, "phases idle at zero duty once aligned at zero demand");
    for _ in 0..50 {
        sim.run_for_ms(1);
        assert_eq!(port(&sim, "TIM1_MOE"), 1.0, "MOE holds asserted across the zero-demand window");
        assert!(max_phase_duty(&sim) < 0.02, "phases stay idle across the zero-demand window");
    }
    assert!(
        read_bool(&sim, "app_motorControl_data.channels[0].isAligned"),
        "alignment is retained across the zero-demand window"
    );

    // --- First dial turn commutates immediately on the stored offset ----------------
    // +90 deg from the arm baseline → command +0.5 → half-scale duty. No second dwell:
    // isAligned stays latched and the drive duty appears within a few control cycles.
    sim.write("vsig:dial:angle[deg]", 90.0).expect("turn the dial");
    sim.run_for_ms(5);
    assert!(
        read_bool(&sim, "app_motorControl_data.channels[0].isAligned"),
        "no second dwell: alignment stays latched through the first dial turn"
    );
    assert_eq!(port(&sim, "TIM1_MOE"), 1.0, "MOE still asserted while commutating");
    let driven = max_phase_duty(&sim);
    assert!(driven > 0.2, "commutation drive appears at once on the first dial turn, got {driven}");

    // --- No fault latched anywhere along the way ------------------------------------
    assert!(!read_bool(&sim, "app_motorControl_data.channels[0].faultLatched"), "no fault latched");
    assert!(
        read_u64(&sim, "app_motorControl_data.channels[0].encoderFaultCount") < 5,
        "encoder fault count stays under the trip limit"
    );

    // Report the duty step for the morning trace: dark → ~0.1 dwell → zero-demand idle →
    // commutating once the dial turns.
    eprintln!(
        "PWM_U_duty step: dark={:.3}, dwell~{:.3}, aligned@{}ms, idle={:.3}, driven={:.3}",
        duty_dark,
        ALIGN_DUTY,
        aligned_at_ms.unwrap(),
        duty_trace.last().copied().unwrap_or(f64::NAN),
        driven
    );
}
