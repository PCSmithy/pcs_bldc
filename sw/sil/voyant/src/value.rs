//! The common value currency for the State Table, plus scalar memory access.
//!
//! Every signal — firmware static (`cvar`), model field (`vsig`), or comms
//! payload — reads/writes through one [`Value`]. That common currency is what
//! lets a heterogeneous registry of `Box<dyn Signal>` stay uniform
//! (state-route-tables.md). Comms `Bytes`/`Record` variants land with the comms
//! layer.

use std::fmt;

/// A scalar leaf type (from a DWARF base/enum type, or a model field).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scalar {
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    Bool,
}

/// A typed scalar value.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value {
    U8(u8),
    U16(u16),
    U32(u32),
    U64(u64),
    I8(i8),
    I16(i16),
    I32(i32),
    I64(i64),
    F32(f32),
    F64(f64),
    Bool(bool),
}

impl Value {
    /// The scalar kind of this value.
    pub fn scalar(self) -> Scalar {
        match self {
            Value::U8(_) => Scalar::U8,
            Value::U16(_) => Scalar::U16,
            Value::U32(_) => Scalar::U32,
            Value::U64(_) => Scalar::U64,
            Value::I8(_) => Scalar::I8,
            Value::I16(_) => Scalar::I16,
            Value::I32(_) => Scalar::I32,
            Value::I64(_) => Scalar::I64,
            Value::F32(_) => Scalar::F32,
            Value::F64(_) => Scalar::F64,
            Value::Bool(_) => Scalar::Bool,
        }
    }
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::U8(x) => write!(f, "{x}"),
            Value::U16(x) => write!(f, "{x}"),
            Value::U32(x) => write!(f, "{x}"),
            Value::U64(x) => write!(f, "{x}"),
            Value::I8(x) => write!(f, "{x}"),
            Value::I16(x) => write!(f, "{x}"),
            Value::I32(x) => write!(f, "{x}"),
            Value::I64(x) => write!(f, "{x}"),
            Value::F32(x) => write!(f, "{x}"),
            Value::F64(x) => write!(f, "{x}"),
            Value::Bool(x) => write!(f, "{x}"),
        }
    }
}

/// Read a scalar of `kind` from `p`.
///
/// SAFETY: `p` points at a valid, readable value of `kind`'s size, and no other
/// thread/context is writing it (the firmware is quiescent during framework
/// access — ffi-boundary.md §5).
pub(crate) unsafe fn read_scalar(p: *const u8, kind: Scalar) -> Value {
    match kind {
        Scalar::U8 => Value::U8(p.read_unaligned()),
        Scalar::U16 => Value::U16((p as *const u16).read_unaligned()),
        Scalar::U32 => Value::U32((p as *const u32).read_unaligned()),
        Scalar::U64 => Value::U64((p as *const u64).read_unaligned()),
        Scalar::I8 => Value::I8((p as *const i8).read_unaligned()),
        Scalar::I16 => Value::I16((p as *const i16).read_unaligned()),
        Scalar::I32 => Value::I32((p as *const i32).read_unaligned()),
        Scalar::I64 => Value::I64((p as *const i64).read_unaligned()),
        Scalar::F32 => Value::F32((p as *const f32).read_unaligned()),
        Scalar::F64 => Value::F64((p as *const f64).read_unaligned()),
        Scalar::Bool => Value::Bool(p.read_unaligned() != 0),
    }
}

/// Write `v` to `p`; `v`'s type must equal `kind`.
///
/// SAFETY: `p` points at a valid, writable location of `kind`'s size, with no
/// concurrent reader/writer (firmware quiescent).
pub(crate) unsafe fn write_scalar(p: *mut u8, kind: Scalar, v: Value) {
    assert_eq!(
        v.scalar(),
        kind,
        "value/field type mismatch: value {:?} vs field {:?}",
        v.scalar(),
        kind
    );
    match v {
        Value::U8(x) => p.write_unaligned(x),
        Value::U16(x) => (p as *mut u16).write_unaligned(x),
        Value::U32(x) => (p as *mut u32).write_unaligned(x),
        Value::U64(x) => (p as *mut u64).write_unaligned(x),
        Value::I8(x) => (p as *mut i8).write_unaligned(x),
        Value::I16(x) => (p as *mut i16).write_unaligned(x),
        Value::I32(x) => (p as *mut i32).write_unaligned(x),
        Value::I64(x) => (p as *mut i64).write_unaligned(x),
        Value::F32(x) => (p as *mut f32).write_unaligned(x),
        Value::F64(x) => (p as *mut f64).write_unaligned(x),
        Value::Bool(x) => p.write_unaligned(x as u8),
    }
}
