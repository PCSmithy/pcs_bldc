//! SplitMix64 against the published reference vector, plus the uniform and
//! Gaussian output contracts.

use prng::Prng;

fn mean(xs: &[f64]) -> f64 {
    xs.iter().sum::<f64>() / (xs.len() as f64)
}

/// Sample standard deviation (Bessel-corrected).
fn std_dev(xs: &[f64]) -> f64 {
    let m = mean(xs);
    let ss: f64 = xs.iter().map(|x| (x - m) * (x - m)).sum();
    (ss / ((xs.len() - 1) as f64)).sqrt()
}

#[test]
fn splitmix64_known_answer() {
    const EXPECTED: [u64; 3] = [0xE220_A839_7B1D_CDAF, 0x6E78_9E6A_A1B9_65F4, 0x06C4_5D18_8009_454F];

    let mut rng = Prng::new(0);
    let got: Vec<u64> = (0..EXPECTED.len()).map(|_| rng.next_u64()).collect();
    assert_eq!(
        got.as_slice(),
        EXPECTED.as_slice(),
        "seed 0 reproduces the published SplitMix64 vector; got {got:016X?}"
    );
}

#[test]
fn next_f64_stays_in_unit_interval() {
    const DRAWS: usize = 10_000;

    let mut rng = Prng::new(0xF64_0001);
    let out: Vec<f64> = (0..DRAWS).map(|_| rng.next_f64()).collect();
    let bad: Vec<f64> = out.iter().copied().filter(|x| !((*x >= 0.0) && (*x < 1.0))).collect();
    assert!(
        bad.is_empty(),
        "every uniform draw lands in [0, 1); {} of {DRAWS} escape: {bad:?}",
        bad.len()
    );
}

#[test]
fn next_gauss_moments() {
    const DRAWS: usize = 100_000;
    const CLT12_BOUND: f64 = 6.0;

    let mut rng = Prng::new(0x6A05_5001);
    let out: Vec<f64> = (0..DRAWS).map(|_| rng.next_gauss()).collect();

    let (m, sd) = (mean(&out), std_dev(&out));
    println!("next_gauss_moments: mean = {m:.6}, std = {sd:.6}");

    let outside = out.iter().filter(|x| x.abs() > CLT12_BOUND).count();
    assert_eq!(outside, 0, "the sum-of-12-uniforms construction bounds every draw to [-6, 6]");
    assert!(m.abs() < 0.02, "the Gaussian is zero-mean: mean = {m:.6}");
    assert!(
        (sd - 1.0).abs() < 0.02,
        "the Gaussian is unit-variance: std = {sd:.6}, expected 1.0 +/-2%"
    );
}
