

pub struct Prng(u64);

impl Prng {
    pub fn new(seed: u64) -> Self { Self(seed) }
    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    /// Uniform in [0, 1): top 53 bits → f64.
    pub fn next_f64(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 * (1.0 / (1u64 << 53) as f64)
    }

    pub fn next_gauss(&mut self) -> f64 {
        let mut s = 0.0;
        for _ in 0..12 { s += self.next_f64(); };
        s - 6.0 // mean 0, variance 1
    }
}

