//! Two firmware instances peer in one world: each `load_firmware` boots its own temp
//! copy of the image under a distinct source name, so the two members mirror into
//! separate `cvar:<board>:…` namespaces and advance independently.

use pcs_bldc_sil::Sil;

#[test]
fn two_firmwares() {
    let mut sim = Sil::new();
    let board_a = sim.load_firmware("board_a");
    let board_b = sim.load_firmware("board_b");
    sim.add_member(board_a);
    sim.add_member(board_b);

    for _ in 0..5 {
        sim.step().expect("engine step");
    }

    let runs = |sim: &Sil, board: &str| sim.read_u64(&format!("cvar:{board}:task1msRuns"));
    let a_runs = runs(&sim, "board_a");
    let b_runs = runs(&sim, "board_b");
    assert!(
        (a_runs > 0) && (b_runs > 0),
        "each board's task1msRuns advances in its own namespace: board_a={a_runs}, board_b={b_runs}"
    );
}
