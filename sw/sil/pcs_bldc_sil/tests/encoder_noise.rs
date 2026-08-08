//! The AS5048 model's measurement-noise term at the SPI wire boundary (no firmware):
//! the noiseless default, seed reproducibility, the residual's moments and whiteness,
//! seed independence, and symmetric wrap at zero.

use pcs_bldc_sil::As5048Model;
use voyant::DuplexPeer;

const READ_ANGLE: [u8; 2] = [0xFF, 0xFF]; // parity 1, read 1, addr 0x3FFF
const TICKS_PER_REV: f64 = 16384.0;
const SIGMA_LSB: f32 = 1.52;
/// Quantizer variance the model's `round()` adds on top of the injected noise.
const QUANTIZER_VAR_LSB2: f64 = 1.0 / 12.0;

/// Decode a response frame into its 14-bit angle, asserting even parity over all 16
/// bits and a clear error flag.
fn decode_angle(frame: &[u8]) -> u16 {
    assert_eq!(
        frame.len(),
        2,
        "response is a 2-byte frame, got {frame:02X?}"
    );
    let f = u16::from_be_bytes([frame[0], frame[1]]);
    assert!(
        f.count_ones().is_multiple_of(2),
        "even parity over frame {f:04X}"
    );
    assert_eq!(f & 0x4000, 0, "error flag clear in frame {f:04X}");
    f & 0x3FFF
}

/// Pump `n` READ-ANGLE polls. The one-frame pipeline means command N is answered in
/// transfer N+1, so a priming transfer absorbs the power-on sentinel.
fn poll_ticks(model: &mut As5048Model, n: usize) -> Vec<u16> {
    let _ = model.transfer(&READ_ANGLE);
    (0..n)
        .map(|_| decode_angle(&model.transfer(&READ_ANGLE)))
        .collect()
}

/// Residuals in LSB about the commanded angle.
fn residuals(ticks: &[u16], angle_lsb: f64) -> Vec<f64> {
    ticks.iter().map(|t| f64::from(*t) - angle_lsb).collect()
}

fn mean(xs: &[f64]) -> f64 {
    xs.iter().sum::<f64>() / (xs.len() as f64)
}

/// Sample standard deviation (Bessel-corrected).
fn std_dev(xs: &[f64]) -> f64 {
    let m = mean(xs);
    let ss: f64 = xs.iter().map(|x| (x - m) * (x - m)).sum();
    (ss / ((xs.len() - 1) as f64)).sqrt()
}

/// Pearson correlation of two equal-length series.
fn pearson(xs: &[f64], ys: &[f64]) -> f64 {
    let (mx, my) = (mean(xs), mean(ys));
    let cov: f64 = xs
        .iter()
        .zip(ys.iter())
        .map(|(x, y)| (x - mx) * (y - my))
        .sum();
    let sx: f64 = xs.iter().map(|x| (x - mx) * (x - mx)).sum::<f64>().sqrt();
    let sy: f64 = ys.iter().map(|y| (y - my) * (y - my)).sum::<f64>().sqrt();
    cov / (sx * sy)
}

#[test]
fn noiseless_default_is_exact() {
    const POLLS: usize = 100;
    const MID_SCALE_TICKS: u16 = 8192; // pi rad, 16384 counts/rev

    let mut model = As5048Model::new("as5048_quiet", std::f32::consts::PI);
    let ticks = poll_ticks(&mut model, POLLS);

    let off: Vec<u16> = ticks
        .iter()
        .copied()
        .filter(|t| *t != MID_SCALE_TICKS)
        .collect();
    assert!(
        off.is_empty(),
        "a model without `with_noise` reports the exact quantized angle on every poll; \
         {} of {POLLS} differ from {MID_SCALE_TICKS}: {off:?}",
        off.len()
    );
}

#[test]
fn same_seed_reproduces_exactly() {
    const POLLS: usize = 1_000;
    const SEED: u64 = 0x5EED_0001;

    let mut a = As5048Model::new("as5048_a", std::f32::consts::PI).with_noise(SIGMA_LSB, SEED);
    let mut b = As5048Model::new("as5048_b", std::f32::consts::PI).with_noise(SIGMA_LSB, SEED);

    let (ta, tb) = (poll_ticks(&mut a, POLLS), poll_ticks(&mut b, POLLS));
    let first_diff = ta.iter().zip(tb.iter()).position(|(x, y)| x != y);
    assert!(
        first_diff.is_none(),
        "two models built with the same (sigma, seed) draw the same stream; \
         first divergence at poll {first_diff:?}"
    );
    assert!(
        ta.iter().any(|t| *t != ta[0]),
        "the noise term actually varies the reported angle across polls"
    );
}

#[test]
fn statistics_match_configured_sigma() {
    const POLLS: usize = 10_000;
    const SEED: u64 = 0x5EED_0003;
    const ANGLE_LSB: f64 = 8192.0;

    let mut model =
        As5048Model::new("as5048_stats", std::f32::consts::PI).with_noise(SIGMA_LSB, SEED);
    let res = residuals(&poll_ticks(&mut model, POLLS), ANGLE_LSB);

    let (m, sd) = (mean(&res), std_dev(&res));
    let expected_sd = (f64::from(SIGMA_LSB) * f64::from(SIGMA_LSB) + QUANTIZER_VAR_LSB2).sqrt();
    println!("statistics_match_configured_sigma: mean = {m:.6}, std = {sd:.6}, expected std = {expected_sd:.6}");

    assert!(
        m.abs() < 0.06,
        "the residual is zero-mean within 4 standard errors: mean = {m:.6} LSB"
    );
    assert!(
        (sd - expected_sd).abs() < (0.04 * expected_sd),
        "the residual spread matches sigma plus the quantizer's 1/12 LSB^2: \
         std = {sd:.6}, expected {expected_sd:.6} +/-4%"
    );
}

#[test]
fn whiteness_lag1() {
    const POLLS: usize = 10_000;
    const SEED: u64 = 0x5EED_0004;
    const ANGLE_LSB: f64 = 8192.0;

    let mut model =
        As5048Model::new("as5048_white", std::f32::consts::PI).with_noise(SIGMA_LSB, SEED);
    let res = residuals(&poll_ticks(&mut model, POLLS), ANGLE_LSB);

    let ac1 = pearson(&res[..POLLS - 1], &res[1..]);
    println!("whiteness_lag1: ac1 = {ac1:.6}");
    assert!(
        ac1.abs() < 0.04,
        "successive samples are uncorrelated: lag-1 autocorrelation = {ac1:.6}"
    );
}

#[test]
fn distinct_seeds_decorrelate() {
    const POLLS: usize = 10_000;
    const SEED_A: u64 = 0x5EED_0005;
    const SEED_B: u64 = 0xA11C_E005;
    const ANGLE_LSB: f64 = 8192.0;

    let mut a =
        As5048Model::new("as5048_seed_a", std::f32::consts::PI).with_noise(SIGMA_LSB, SEED_A);
    let mut b =
        As5048Model::new("as5048_seed_b", std::f32::consts::PI).with_noise(SIGMA_LSB, SEED_B);

    let (ta, tb) = (poll_ticks(&mut a, POLLS), poll_ticks(&mut b, POLLS));
    assert_ne!(ta, tb, "distinct seeds produce distinct streams");

    let r = pearson(&residuals(&ta, ANGLE_LSB), &residuals(&tb, ANGLE_LSB));
    println!("distinct_seeds_decorrelate: pearson r = {r:.6}");
    assert!(
        r.abs() < 0.04,
        "streams from distinct seeds are uncorrelated: Pearson r = {r:.6}"
    );
}

#[test]
fn wraparound_at_zero() {
    const POLLS: usize = 10_000;
    const SEED: u64 = 0x5EED_0006;
    const LOW_MAX: u16 = 100;
    const HIGH_MIN: u16 = 16_284;

    let mut model = As5048Model::new("as5048_wrap", 0.0).with_noise(SIGMA_LSB, SEED);
    let ticks = poll_ticks(&mut model, POLLS);

    let mid: Vec<u16> = ticks
        .iter()
        .copied()
        .filter(|t| (*t >= LOW_MAX) && (*t <= HIGH_MIN))
        .collect();
    assert!(
        mid.is_empty(),
        "noise about zero stays within a few LSB of the wrap point; {} mid-range samples: {mid:?}",
        mid.len()
    );

    let low = ticks.iter().filter(|t| **t < LOW_MAX).count();
    let low_fraction = (low as f64) / (POLLS as f64);
    // Signed residual about zero: the upper half of the circle reads as negative.
    let signed: Vec<f64> = ticks
        .iter()
        .map(|t| {
            let v = f64::from(*t);
            match v > (TICKS_PER_REV / 2.0) {
                true => v - TICKS_PER_REV,
                false => v,
            }
        })
        .collect();
    let m = mean(&signed);
    println!("wraparound_at_zero: low fraction = {low_fraction:.4}, high fraction = {:.4}, circular mean = {m:.6}", 1.0 - low_fraction);

    assert!(
        (low > 0) && (low < POLLS),
        "both sides of the wrap point occur: {low} low of {POLLS}"
    );
    assert!(
        (low_fraction > 0.25) && (low_fraction < 0.75),
        "the wrap is symmetric rather than clamped: low-side fraction = {low_fraction:.4}"
    );
    assert!(
        m.abs() < 0.06,
        "the circular mean sits on the commanded angle: mean = {m:.6} LSB"
    );
}
