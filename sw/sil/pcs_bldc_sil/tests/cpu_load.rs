mod common;

use common::u64_at;
use pcs_bldc_sil::board::board;
use pcs_bldc_sil::Board;

const SLOT_COUNT: usize = 8;
const SLOT_IDLE: usize = 1;

fn slots(sim: &pcs_bldc_sil::Sil) -> [u64; SLOT_COUNT] {
    let mut out = [0u64; SLOT_COUNT];
    for (i, slot) in out.iter_mut().enumerate() {
        *slot = u64_at(sim, &format!("app_cpuLoad_data.taskTime_us[{i}]"));
    }
    out
}

// [test->fw~obs_cpu_001~1]
#[test]
fn the_slots_partition_the_elapsed_time() {
    // The sim clock advances between engine steps, so a run that stays inside
    // one step measures zero and the time lands on whichever task was in when
    // the clock moved: the idle task, every task being blocked between ticks.
    // The partition itself is what the firmware promises; the split is bench.
    let Board { mut sim, .. } = board(0.0);
    sim.run_for_ms(200);
    let before = slots(&sim);
    sim.run_for_ms(1000);
    let after = slots(&sim);

    let deltas: Vec<u64> = before
        .iter()
        .zip(after.iter())
        .map(|(b, a)| a.wrapping_sub(*b) & 0xFFFF_FFFF)
        .collect();
    let sum: u64 = deltas.iter().sum();
    let elapsed_us = 1_000_000u64;
    let err = (sum as f64 - elapsed_us as f64).abs() / elapsed_us as f64;
    println!("cpu_load: slot deltas {deltas:?}, sum {sum} us over {elapsed_us} us ({:.2}% off)", err * 100.0);
    assert!(err < 0.01, "slots sum to the elapsed time within 1%: {sum} vs {elapsed_us}");
    assert!(
        deltas[SLOT_IDLE] * 10 >= sum * 9,
        "the idle slot carries the sim's blocked time: {} of {sum}",
        deltas[SLOT_IDLE]
    );
}
