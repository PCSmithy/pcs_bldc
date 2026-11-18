//! The PWM timer's output lines on the board world under the field-oriented drive:
//! each complementary channel's CHx / CHxN pair with the configured dead-time
//! (fw~hal_tim_005), and the break input forcing every line inactive until the
//! master output enable is set again (fw~hal_tim_007).
//!
//! The sim TIM derives each line's edges over a counting cycle from the active
//! compare; the checks sample those edges into line levels across the cycle and
//! measure the gaps, so they hold the lines to the spec, not to the derivation.

mod common;

use common::proto::{clear_fault, command, speed_setpoint};
use common::{accepted, bool_at, exchange, select_method, set_bool, tap_button, GRID_US, MC, MODE_FOC_VELOCITY};
use pcs_bldc_sil::board::{align_dwell_ms, board_with_motor, fault_latched, port, GATE_BRINGUP_MS};
use pcs_bldc_sil::{MotorParams, Sil, SilOptions};
use voyant::Value;

/// TIM1, the bridge PWM peripheral, and its three complementary phase channels.
const TIM1: &str = "HW_TIM_data.peripheralData[0]";
const PHASES: [(usize, &str); 3] = [(0, "U"), (1, "V"), (2, "W")];
const BREAK_INPUT: &str = "HW_TIM_data.breakInput[0]";

/// The board's TIM1 dead-time: BDTR.DTG = 43 at clock division 1, 43 timer-clock
/// ticks. Stated here rather than read back, so a wrong board value fails.
const DEAD_TIME_TICKS: u64 = 51;
/// The board's TIM1 period (center-aligned, ARR = 4250): 8500 ticks a cycle.
const PERIOD: u64 = 4250;

fn u64_at(sim: &Sil, path: &str) -> u64 {
    sim.fw().read_cvar(path).as_u64().unwrap_or_else(|| panic!("{path} reads as a count"))
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum State {
    Inactive,
    Active,
    Switching,
}

#[derive(Debug, Clone, Copy)]
struct Line {
    state: State,
    on: u64,
    off: u64,
}

impl Line {
    /// The line's level at `t` ticks into the cycle.
    fn active_at(&self, t: u64) -> bool {
        match self.state {
            State::Inactive => false,
            State::Active => true,
            State::Switching if self.on < self.off => (t >= self.on) && (t < self.off),
            State::Switching => (t >= self.on) || (t < self.off),
        }
    }
}

fn line(sim: &Sil, which: &str, unit: usize) -> Line {
    let base = format!("{TIM1}.{which}[{unit}]");
    // HW_TIM_lineState_E, by enumerator name or (no name in the image) ordinal.
    let state = match sim.fw().read_cvar(&format!("{base}.state")) {
        Value::Enum(name) => match name.as_str() {
            "HW_TIM_LINE_INACTIVE" | "<0>" => State::Inactive,
            "HW_TIM_LINE_ACTIVE" | "<1>" => State::Active,
            "HW_TIM_LINE_SWITCHING" | "<2>" => State::Switching,
            other => panic!("{base}.state reads as a line state, got {other}"),
        },
        other => panic!("{base}.state reads as a line state, got {other:?}"),
    };
    Line { state, on: u64_at(sim, &format!("{base}.onAt")), off: u64_at(sim, &format!("{base}.offAt")) }
}

/// One complementary pair's levels over a cycle, measured: the ticks from each
/// line's deactivation to its complement's activation, both directions, and the
/// primary's active ticks. Panics if both lines are ever active together.
struct PairMeasure {
    high_to_low_gap: u64,
    low_to_high_gap: u64,
    high_on: u64,
}

fn measure_pair(high: &Line, low: &Line, cycle: u64) -> PairMeasure {
    let level = |l: &Line, t: u64| l.active_at(t % cycle);
    let mut high_on = 0;
    for t in 0..cycle {
        assert!(!(level(high, t) && level(low, t)), "both lines active at tick {t}: {high:?} {low:?}");
        if level(high, t) {
            high_on += 1;
        }
    }
    // From each falling edge, count the ticks with both lines inactive until the
    // complement rises.
    let gap_after_fall = |from: &Line, to: &Line| -> u64 {
        let fall = (0..cycle).find(|&t| level(from, t) && !level(from, t + 1)).expect("the line falls") + 1;
        (0..cycle).take_while(|&d| !level(to, fall + d)).count() as u64
    };
    PairMeasure { high_to_low_gap: gap_after_fall(high, low), low_to_high_gap: gap_after_fall(low, high), high_on }
}

/// The board world on the 50 us grid, aligned and driving a 5 rad/s velocity target.
fn driving() -> Sil {
    let mut board = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, MotorParams::default());
    let sim = &mut board.sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(sim, MODE_FOC_VELOCITY);
    tap_button(sim);
    sim.run_for_ms(align_dwell_ms(sim) + 100);
    assert!(bool_at(sim, &format!("{MC}.isAligned")), "aligned");
    let (reply, _) = exchange(sim, 1, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, 5.0, 0.0)]));
    assert!(accepted(&reply), "the speed target is accepted");
    sim.run_for_ms(100);
    assert!(!fault_latched(sim), "driving, no fault");
    board.sim
}

/// Every phase pair is switching, and its primary follows the active compare: a
/// center-aligned PWM-1 reference is active for 2C - 1 ticks, less the dead-time.
fn assert_lines_follow_compares(sim: &Sil, why: &str) {
    let cycle = u64_at(sim, &format!("{TIM1}.cycleTicks"));
    for (unit, name) in PHASES {
        let high = line(sim, "primaryLine", unit);
        let low = line(sim, "complementLine", unit);
        let compare = u64_at(sim, &format!("{TIM1}.activeCompare[{unit}]"));
        assert!(compare > DEAD_TIME_TICKS, "{why}: phase {name} compare {compare} is mid-range");
        let m = measure_pair(&high, &low, cycle);
        assert_eq!(m.high_on, 2 * compare - 1 - DEAD_TIME_TICKS, "{why}: phase {name} high side follows compare {compare}");
    }
}

fn assert_all_lines_inactive(sim: &Sil, why: &str) {
    for (unit, name) in PHASES {
        for which in ["primaryLine", "complementLine"] {
            assert_eq!(line(sim, which, unit).state, State::Inactive, "{why}: phase {name} {which}");
        }
    }
}

fn moe_latch(sim: &Sil) -> bool {
    bool_at(sim, &format!("{TIM1}.mainOutputEnabled"))
}

// [test->fw~hal_tim_005~1]
#[test]
fn each_phase_pair_is_antiphase_with_the_dead_time_in_both_directions() {
    let mut sim = driving();
    let cycle = u64_at(&sim, &format!("{TIM1}.cycleTicks"));
    assert_eq!(cycle, 2 * PERIOD, "a center-aligned cycle is two periods of ticks");
    let ticks_per_us = u64_at(&sim, "HW_TIM_peripheralConfig[0].countsPerUs");

    // Each PWM cycle for 20 ms, every phase: both lines switch, never overlap,
    // and each activation trails its complement's deactivation by the dead-time.
    let (mut lo, mut hi) = (u64::MAX, 0);
    for _ in 0..400 {
        sim.step().expect("engine step");
        for (unit, name) in PHASES {
            let high = line(&sim, "primaryLine", unit);
            let low = line(&sim, "complementLine", unit);
            assert_eq!((high.state, low.state), (State::Switching, State::Switching), "phase {name} switches");
            let m = measure_pair(&high, &low, cycle);
            assert_eq!(m.high_to_low_gap, DEAD_TIME_TICKS, "phase {name}: high off -> low on");
            assert_eq!(m.low_to_high_gap, DEAD_TIME_TICKS, "phase {name}: low off -> high on");
            let compare = u64_at(&sim, &format!("{TIM1}.activeCompare[{unit}]"));
            lo = lo.min(compare);
            hi = hi.max(compare);
        }
    }
    println!(
        "hal_tim_outputs: {DEAD_TIME_TICKS} ticks = {:.1} ns dead band both ways on U/V/W over compares {lo}..{hi} of {PERIOD}",
        DEAD_TIME_TICKS as f64 * 1000.0 / ticks_per_us as f64
    );
    assert!(hi > lo, "the drive swept the compares");
}

// [test->fw~hal_tim_007~1]
// [test->sys~mc_004~1]
#[test]
fn a_break_holds_every_line_inactive_until_the_enable_is_set_again() {
    let mut sim = driving();
    assert!(moe_latch(&sim), "the drive has the master output enabled");
    assert_lines_follow_compares(&sim, "before the break");

    // Assert: the next step has every line inactive and the enable clear.
    set_bool(&sim, BREAK_INPUT, true);
    sim.step().expect("engine step");
    assert!(!moe_latch(&sim), "the break clears the master output enable");
    assert_eq!(port(&sim, "TIM1_MOE"), 0.0, "the published enable is off");
    assert_all_lines_inactive(&sim, "on assert");
    // The cycle interrupt keeps writing duties until the 1 ms pass reads the
    // enable off: the compares move while every line holds inactive.
    let compares = |sim: &Sil| PHASES.map(|(unit, _)| u64_at(sim, &format!("{TIM1}.compare[{unit}]")));
    let mut last = compares(&sim);
    let (mut changes, mut last_change) = (0u32, 0u32);
    for k in 1..=(20 * 20) {
        sim.step().expect("engine step");
        assert!(!moe_latch(&sim), "step {k}: held while asserted");
        assert_all_lines_inactive(&sim, &format!("step {k} under the break"));
        let now = compares(&sim);
        if now != last {
            changes += 1;
            last_change = k;
        }
        last = now;
    }
    assert!(fault_latched(&sim), "the firmware saw the break");
    println!("hal_tim_outputs: under the break the duties changed on {changes} cycles (last at cycle {last_change}), all six lines inactive");
    assert!(changes >= 3, "duty commands continued under the break: {changes} changes");

    // Still asserted: the firmware clears its fault and commands the enable,
    // which does not set — the firmware reads it off and latches again.
    let (reply, _) = exchange(&mut sim, 2, clear_fault());
    assert!(accepted(&reply), "ClearFault is accepted");
    let (reply, _) = exchange(&mut sim, 3, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, 5.0, 0.0)]));
    assert!(accepted(&reply), "the enable request is accepted");
    for k in 1..=(20 * 20) {
        sim.step().expect("engine step");
        assert!(!moe_latch(&sim), "step {k}: a commanded enable does not set under the break");
        assert_all_lines_inactive(&sim, "commanded under the break");
    }
    assert!(fault_latched(&sim), "the commanded enable read back off and latched the fault");

    // Released: the enable stays clear until software sets it.
    set_bool(&sim, BREAK_INPUT, false);
    for k in 1..=(20 * 20) {
        sim.step().expect("engine step");
        assert!(!moe_latch(&sim), "step {k}: released, the enable stays clear");
    }
    assert_all_lines_inactive(&sim, "released, not re-enabled");

    // Set again: the lines follow the compares.
    let (reply, _) = exchange(&mut sim, 4, clear_fault());
    assert!(accepted(&reply), "ClearFault is accepted");
    let (reply, _) = exchange(&mut sim, 5, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, 5.0, 0.0)]));
    assert!(accepted(&reply), "the enable request is accepted");
    sim.run_for_ms(20);
    assert!(moe_latch(&sim) && !fault_latched(&sim), "the enable set with the break released");
    assert_eq!(port(&sim, "TIM1_MOE"), 1.0, "the published enable is on");
    assert_lines_follow_compares(&sim, "re-enabled");
    println!("hal_tim_outputs: break held all six lines inactive through a commanded enable; restored on the next enable");
}
