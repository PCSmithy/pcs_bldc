//! Signal identity + value representation.
//!
//! [`SignalId`] is the canonical key `sig_type:source:name[:modifier]`, stored
//! as an owned string with validation + accessors. [`Value`] is the framework's
//! common value currency — a *logical* type (not a 1:1 mirror of C widths); the
//! cvar sample-resolver coerces firmware scalar widths into it.

use serde::{Deserialize, Serialize};
use std::fmt;
use thiserror::Error;

/// Segment names, by index (a future `tool` segment could prepend this).
const SEGMENT_NAMES: [&str; 4] = ["sig_type", "source", "name", "modifier"];

/// Recognized `sig_type` values. `spi` is the first comms bus — its transactions
/// land as `tx`/`rx` event records (`Value::Bytes`); the rest (`usb_cdc`/`i2c`/…)
/// join as they land.
pub const APPROVED_SIG_TYPES: &[&str] = &["cvar", "vsig", "spi"];

/// The comms `sig_type`s that carry duplex (request/response) transactions — a
/// duplex endpoint's `sig_type` must be one of these. `spi` is the first; `i2c` /
/// `uart` join as their bus drivers land.
pub const DUPLEX_BUS_SIG_TYPES: &[&str] = &["spi"];

/// Whether `sig_type` names a duplex transaction bus (see [`DUPLEX_BUS_SIG_TYPES`]).
pub fn is_duplex_bus(sig_type: &str) -> bool {
    DUPLEX_BUS_SIG_TYPES.contains(&sig_type)
}

#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum ParseError {
    #[error("signal id needs 3 or 4 `:`-segments (sig_type:source:name[:modifier]), got {0}")]
    WrongSegmentCount(usize),
    #[error("segment {index} ({name}) must not be empty")]
    EmptySegment { index: usize, name: &'static str },
    #[error("unknown sig_type {0:?} (approved: {APPROVED_SIG_TYPES:?})")]
    UnknownSigType(String),
}

/// A structured signal identifier: `sig_type:source:name[:modifier]`.
///
/// `:` is the delimiter; the `name` (a DWARF path for `cvar`, e.g.
/// `sensor_data.channel[0].counts[6]`) never contains `:`.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct SignalId(String);

impl SignalId {
    /// Parse + validate a canonical id string.
    pub fn parse(s: &str) -> Result<Self, ParseError> {
        let segs: Vec<&str> = s.split(':').collect();
        if segs.len() < 3 || segs.len() > 4 {
            return Err(ParseError::WrongSegmentCount(segs.len()));
        }
        for (i, seg) in segs.iter().enumerate() {
            if seg.is_empty() {
                return Err(ParseError::EmptySegment {
                    index: i,
                    name: SEGMENT_NAMES[i],
                });
            }
        }
        if !APPROVED_SIG_TYPES.contains(&segs[0]) {
            return Err(ParseError::UnknownSigType(segs[0].to_string()));
        }
        Ok(SignalId(s.to_string()))
    }

    /// Build from parts (validated).
    pub fn new(
        sig_type: &str,
        source: &str,
        name: &str,
        modifier: Option<&str>,
    ) -> Result<Self, ParseError> {
        let s = match modifier {
            Some(m) => format!("{sig_type}:{source}:{name}:{m}"),
            None => format!("{sig_type}:{source}:{name}"),
        };
        Self::parse(&s)
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    fn seg(&self, i: usize) -> Option<&str> {
        self.0.split(':').nth(i)
    }

    // Accessors are infallible for the first three: `parse` guarantees them.
    pub fn sig_type(&self) -> &str {
        self.seg(0).unwrap()
    }
    pub fn source(&self) -> &str {
        self.seg(1).unwrap()
    }
    pub fn name(&self) -> &str {
        self.seg(2).unwrap()
    }
    pub fn modifier(&self) -> Option<&str> {
        self.seg(3)
    }
}

impl fmt::Display for SignalId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// The framework's common value currency. Logical types: signed ints collapse
/// to `I32`, unsigned to `U32`/`U64`, plus float/bool, symbolic `Enum`, and
/// `Bytes` for comms payloads. (Firmware widths are coerced by the cvar
/// resolver; see backend.)
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Value {
    F32(f32),
    F64(f64),
    I32(i32),
    U32(u32),
    U64(u64),
    Bool(bool),
    Enum(String),
    Bytes(Vec<u8>),
}

impl Value {
    // Decoders paired with the `From` constructors: extract a native scalar,
    // coercing within the numeric family; `None` = wrong variant for the ask.
    // Only the shapes with consumers — add more when a call shape needs them.

    /// Decode to `f32` (`F64` narrows lossily — model working precision).
    pub fn as_f32(&self) -> Option<f32> {
        match self {
            Value::F32(x) => Some(*x),
            Value::F64(x) => Some(*x as f32),
            _ => None,
        }
    }

    /// Decode to `f64` (the float family, losslessly).
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::F32(x) => Some(f64::from(*x)),
            Value::F64(x) => Some(*x),
            _ => None,
        }
    }

    /// Decode to `u64` (unsigned family + `Bool`; a negative `I32` is `None`).
    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Value::U32(x) => Some(u64::from(*x)),
            Value::U64(x) => Some(*x),
            Value::I32(x) => u64::try_from(*x).ok(),
            Value::Bool(b) => Some(u64::from(*b)),
            _ => None,
        }
    }

    /// Change comparison for the historian: floats within `epsilon` (absolute),
    /// everything else exact. Different variants always compare unequal.
    pub fn approx_eq(&self, other: &Self, epsilon: f64) -> bool {
        match (self, other) {
            (Value::F32(a), Value::F32(b)) => ((*a as f64) - (*b as f64)).abs() <= epsilon,
            (Value::F64(a), Value::F64(b)) => (a - b).abs() <= epsilon,
            (Value::I32(a), Value::I32(b)) => a == b,
            (Value::U32(a), Value::U32(b)) => a == b,
            (Value::U64(a), Value::U64(b)) => a == b,
            (Value::Bool(a), Value::Bool(b)) => a == b,
            (Value::Enum(a), Value::Enum(b)) => a == b,
            (Value::Bytes(a), Value::Bytes(b)) => a == b,
            _ => false,
        }
    }
}

// `From` conversions so table writes take bare literals (`write(id, true)`,
// `write(id, 90.0)`). Only the variants the scenario API actually drives — bool /
// u32 / u64 / f64 — are provided; add more when a call shape needs them.
impl From<bool> for Value {
    fn from(x: bool) -> Self {
        Value::Bool(x)
    }
}
impl From<u32> for Value {
    fn from(x: u32) -> Self {
        Value::U32(x)
    }
}
impl From<u64> for Value {
    fn from(x: u64) -> Self {
        Value::U64(x)
    }
}
impl From<f64> for Value {
    fn from(x: f64) -> Self {
        Value::F64(x)
    }
}
impl From<Vec<u8>> for Value {
    fn from(x: Vec<u8>) -> Self {
        Value::Bytes(x)
    }
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::F32(x) => write!(f, "{x}"),
            Value::F64(x) => write!(f, "{x}"),
            Value::I32(x) => write!(f, "{x}"),
            Value::U32(x) => write!(f, "{x}"),
            Value::U64(x) => write!(f, "{x}"),
            Value::Bool(x) => write!(f, "{x}"),
            Value::Enum(x) => write!(f, "{x}"),
            Value::Bytes(b) => write!(f, "<{} bytes>", b.len()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn as_decoders_coerce_within_family_and_refuse_across() {
        assert_eq!(Value::F64(90.0).as_f32(), Some(90.0f32));
        assert_eq!(Value::F32(1.5).as_f64(), Some(1.5f64));
        assert_eq!(Value::U32(7).as_u64(), Some(7));
        assert_eq!(Value::I32(-1).as_u64(), None); // negative refuses, not wraps
        assert_eq!(Value::Bool(true).as_u64(), Some(1));
        assert_eq!(Value::U32(7).as_f32(), None); // no cross-family coercion
        assert_eq!(Value::Bytes(vec![1]).as_f64(), None);
    }

    #[test]
    fn parses_cvar_without_modifier() {
        let id = SignalId::parse("cvar:dut:sensor_data.channel[0].counts[6]").unwrap();
        assert_eq!(id.sig_type(), "cvar");
        assert_eq!(id.source(), "dut");
        assert_eq!(id.name(), "sensor_data.channel[0].counts[6]");
        assert_eq!(id.modifier(), None);
    }

    #[test]
    fn parses_with_modifier() {
        let id = SignalId::parse("vsig:motor:phase_u_voltage:out").unwrap();
        assert_eq!(id.name(), "phase_u_voltage");
        assert_eq!(id.modifier(), Some("out"));
    }

    #[test]
    fn rejects_bad_ids() {
        assert!(matches!(
            SignalId::parse("cvar:onlytwo"),
            Err(ParseError::WrongSegmentCount(2))
        ));
        assert!(matches!(
            SignalId::parse("cvar::name"),
            Err(ParseError::EmptySegment { index: 1, .. })
        ));
        assert!(matches!(
            SignalId::parse("bogus:src:name"),
            Err(ParseError::UnknownSigType(_))
        ));
    }

    #[test]
    fn new_roundtrips() {
        let id = SignalId::new("cvar", "dut", "x.y", None).unwrap();
        assert_eq!(id.as_str(), "cvar:dut:x.y");
    }

    #[test]
    fn approx_eq_floats_and_exact() {
        assert!(Value::F32(1.0).approx_eq(&Value::F32(1.0005), 1e-3));
        assert!(!Value::F32(1.0).approx_eq(&Value::F32(1.01), 1e-3));
        assert!(Value::U32(5).approx_eq(&Value::U32(5), 1e-3));
        assert!(!Value::U32(5).approx_eq(&Value::U32(6), 1e-3));
        assert!(!Value::U32(5).approx_eq(&Value::I32(5), 1e-3)); // different variant
    }
}
