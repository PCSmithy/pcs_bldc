//! Trace serializer: the State Table historian → a versioned little-endian binary
//! stream (D12 §7). Pure Rust — no Python, no subprocess. The instantiation spawns a
//! builder (`tools/mf4_build.py`, asammdf) that turns this stream into ASAM MDF4.
//!
//! ## Wire format (all integers little-endian)
//!
//! ```text
//! header : magic b"VYTR" | version u32 (=2) | end_time_us u64 | signal_count u32
//! signal : id_len u32 | id utf8
//!          unit_len u32 | unit utf8   (empty = no unit)
//!          dtype u8                   (see the DT_* tags)
//!          sample_count u64
//!          timestamps  : sample_count × u64 (sim-time µs)
//!          values      : sample_count × native scalar
//!                        (bool → u8 0/1; enum → u32 ordinal)
//!          enum table  : ONLY for dtype=DT_ENUM —
//!                        entry_count u32, then entry_count × (ordinal u32, name)
//! ```
//!
//! `Bytes`-typed signals and never-recorded (empty) signals are skipped — neither is
//! a plottable scalar. Enum values are stored by *name* in the historian
//! ([`Value::Enum`]), so each signal's distinct names are assigned first-appearance
//! ordinals here and the value→name table travels alongside for the reader's MDF
//! value-to-text conversion.

use crate::signal::{SignalId, Value};
use crate::state_table::StateTable;
use std::io::{self, Write};

/// Stream magic (greppable).
pub const MAGIC: &[u8; 4] = b"VYTR";
/// Framing format version. v2 adds the run end time (u64 µs) after the version,
/// so a ZOH-materializing reader can extend every signal to the end of the run.
pub const FORMAT_VERSION: u32 = 2;

// dtype tags (u8).
pub const DT_F32: u8 = 0;
pub const DT_F64: u8 = 1;
pub const DT_I32: u8 = 2;
pub const DT_U32: u8 = 3;
pub const DT_U64: u8 = 4;
pub const DT_BOOL: u8 = 5;
pub const DT_ENUM: u8 = 6;

/// Serialize the historian to `w`. `prefix_filter`: keep only signals whose id
/// starts with one of the prefixes; `None` = every signal. See the module docs for
/// the wire format.
pub fn write_trace<W: Write>(
    st: &StateTable,
    w: &mut W,
    prefix_filter: Option<&[&str]>,
) -> io::Result<()> {
    // Resolve the emit list first (need the count up front): apply the prefix
    // filter, drop never-recorded (empty) and Bytes signals.
    let mut emit: Vec<(&SignalId, Vec<(u64, Value)>)> = Vec::new();
    for id in st.signals() {
        if let Some(prefixes) = prefix_filter {
            if !prefixes.iter().any(|p| id.as_str().starts_with(p)) {
                continue;
            }
        }
        let samples = st.changes(id).unwrap_or_default();
        match samples.first() {
            None => continue,                              // never recorded
            Some((_, Value::Bytes(_))) => continue,        // not a plottable scalar
            Some(_) => {}
        }
        emit.push((id, samples));
    }

    w.write_all(MAGIC)?;
    write_u32(w, FORMAT_VERSION)?;
    write_u64(w, st.now_us())?;
    write_u32(w, emit.len() as u32)?;
    for (id, samples) in &emit {
        write_signal(w, id.as_str(), st.unit_of(id), samples)?;
    }
    Ok(())
}

fn write_signal<W: Write>(
    w: &mut W,
    id: &str,
    unit: Option<&str>,
    samples: &[(u64, Value)],
) -> io::Result<()> {
    let dtype = dtype_tag(&samples[0].1);
    write_str(w, id)?;
    write_str(w, unit.unwrap_or(""))?;
    w.write_all(&[dtype])?;
    write_u64(w, samples.len() as u64)?;
    for (t, _) in samples {
        write_u64(w, *t)?;
    }
    if dtype == DT_ENUM {
        // The historian keys enums by name; assign first-appearance ordinals and
        // carry the value→name table for the reader's MDF value-to-text conversion.
        let mut names: Vec<&str> = Vec::new();
        for (_, v) in samples {
            let Value::Enum(s) = v else { unreachable!("enum column holds only Enum") };
            let ord = names.iter().position(|n| *n == s.as_str()).unwrap_or_else(|| {
                names.push(s.as_str());
                names.len() - 1
            });
            write_u32(w, ord as u32)?;
        }
        write_u32(w, names.len() as u32)?;
        for (ord, name) in names.iter().enumerate() {
            write_u32(w, ord as u32)?;
            write_str(w, name)?;
        }
    } else {
        for (_, v) in samples {
            write_value(w, v)?;
        }
    }
    Ok(())
}

/// The dtype tag for a column, read off its (uniform, one-type-rule) first sample.
fn dtype_tag(v: &Value) -> u8 {
    match v {
        Value::F32(_) => DT_F32,
        Value::F64(_) => DT_F64,
        Value::I32(_) => DT_I32,
        Value::U32(_) => DT_U32,
        Value::U64(_) => DT_U64,
        Value::Bool(_) => DT_BOOL,
        Value::Enum(_) => DT_ENUM,
        Value::Bytes(_) => unreachable!("Bytes skipped before write_signal"),
    }
}

/// One native scalar, little-endian (bool → u8). Enum/Bytes never reach here.
fn write_value<W: Write>(w: &mut W, v: &Value) -> io::Result<()> {
    match v {
        Value::F32(x) => w.write_all(&x.to_le_bytes()),
        Value::F64(x) => w.write_all(&x.to_le_bytes()),
        Value::I32(x) => w.write_all(&x.to_le_bytes()),
        Value::U32(x) => w.write_all(&x.to_le_bytes()),
        Value::U64(x) => w.write_all(&x.to_le_bytes()),
        Value::Bool(b) => w.write_all(&[u8::from(*b)]),
        Value::Enum(_) | Value::Bytes(_) => unreachable!("handled/skipped by write_signal"),
    }
}

fn write_u32<W: Write>(w: &mut W, x: u32) -> io::Result<()> {
    w.write_all(&x.to_le_bytes())
}

fn write_u64<W: Write>(w: &mut W, x: u64) -> io::Result<()> {
    w.write_all(&x.to_le_bytes())
}

fn write_str<W: Write>(w: &mut W, s: &str) -> io::Result<()> {
    write_u32(w, s.len() as u32)?;
    w.write_all(s.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn id(s: &str) -> SignalId {
        SignalId::parse(s).unwrap()
    }

    // --- a hand decoder, mirroring the wire format independently of the writer ---

    #[derive(Debug, PartialEq)]
    struct DecodedSignal {
        id: String,
        unit: String,
        dtype: u8,
        times: Vec<u64>,
        /// Raw native values, decoded per dtype into the widest faithful form.
        values: Vec<DecodedValue>,
        /// (ordinal, name) pairs — empty unless `dtype == DT_ENUM`.
        enum_table: Vec<(u32, String)>,
    }

    #[derive(Debug, PartialEq)]
    enum DecodedValue {
        F32(f32),
        F64(f64),
        I32(i32),
        U32(u32),
        U64(u64),
        Bool(bool),
        Enum(u32),
    }

    struct Reader<'a> {
        b: &'a [u8],
        o: usize,
    }

    impl<'a> Reader<'a> {
        fn take(&mut self, n: usize) -> &'a [u8] {
            let s = &self.b[self.o..self.o + n];
            self.o += n;
            s
        }
        fn u8(&mut self) -> u8 {
            self.take(1)[0]
        }
        fn u32(&mut self) -> u32 {
            u32::from_le_bytes(self.take(4).try_into().unwrap())
        }
        fn u64(&mut self) -> u64 {
            u64::from_le_bytes(self.take(8).try_into().unwrap())
        }
        fn str(&mut self) -> String {
            let n = self.u32() as usize;
            String::from_utf8(self.take(n).to_vec()).unwrap()
        }
    }

    fn decode(bytes: &[u8]) -> Vec<DecodedSignal> {
        let mut r = Reader { b: bytes, o: 0 };
        assert_eq!(r.take(4), MAGIC, "magic");
        assert_eq!(r.u32(), FORMAT_VERSION, "version");
        let _end_time_us = r.u64();
        let count = r.u32();
        let mut out = Vec::new();
        for _ in 0..count {
            let id = r.str();
            let unit = r.str();
            let dtype = r.u8();
            let n = r.u64() as usize;
            let times: Vec<u64> = (0..n).map(|_| r.u64()).collect();
            let values: Vec<DecodedValue> = (0..n)
                .map(|_| match dtype {
                    DT_F32 => DecodedValue::F32(f32::from_le_bytes(r.take(4).try_into().unwrap())),
                    DT_F64 => DecodedValue::F64(f64::from_le_bytes(r.take(8).try_into().unwrap())),
                    DT_I32 => DecodedValue::I32(i32::from_le_bytes(r.take(4).try_into().unwrap())),
                    DT_U32 => DecodedValue::U32(r.u32()),
                    DT_U64 => DecodedValue::U64(r.u64()),
                    DT_BOOL => DecodedValue::Bool(r.u8() != 0),
                    DT_ENUM => DecodedValue::Enum(r.u32()),
                    other => panic!("bad dtype {other}"),
                })
                .collect();
            let enum_table = if dtype == DT_ENUM {
                let m = r.u32();
                (0..m).map(|_| (r.u32(), r.str())).collect()
            } else {
                Vec::new()
            };
            out.push(DecodedSignal { id, unit, dtype, times, values, enum_table });
        }
        assert_eq!(r.o, bytes.len(), "trailing bytes");
        out
    }

    fn dump(st: &StateTable, filter: Option<&[&str]>) -> Vec<u8> {
        let mut buf = Vec::new();
        write_trace(st, &mut buf, filter).unwrap();
        buf
    }

    #[test]
    fn empty_table_round_trips_to_a_header_only_stream() {
        let st = StateTable::new();
        let bytes = dump(&st, None);
        assert_eq!(&bytes[..4], MAGIC);
        assert!(decode(&bytes).is_empty());
    }

    #[test]
    fn scalar_and_bool_signals_round_trip() {
        let mut st = StateTable::new();
        let f = id("vsig:m:angle");
        let b = id("cvar:dut:flag");
        st.register(f.clone(), Some("rad")).unwrap();
        st.register(b.clone(), None).unwrap();
        st.set_time(1_000);
        st.record(&f, Value::F64(1.5)).unwrap();
        st.record(&b, Value::Bool(false)).unwrap();
        st.set_time(2_000);
        st.record(&f, Value::F64(2.5)).unwrap();
        st.record(&b, Value::Bool(true)).unwrap();

        let decoded = decode(&dump(&st, None));
        let sf = decoded.iter().find(|d| d.id == "vsig:m:angle").unwrap();
        assert_eq!(sf.unit, "rad");
        assert_eq!(sf.dtype, DT_F64);
        assert_eq!(sf.times, vec![1_000, 2_000]);
        assert_eq!(sf.values, vec![DecodedValue::F64(1.5), DecodedValue::F64(2.5)]);

        let sb = decoded.iter().find(|d| d.id == "cvar:dut:flag").unwrap();
        assert_eq!(sb.unit, ""); // no unit
        assert_eq!(sb.dtype, DT_BOOL);
        assert_eq!(sb.values, vec![DecodedValue::Bool(false), DecodedValue::Bool(true)]);
    }

    #[test]
    fn enum_signal_carries_ordinals_and_a_value_to_text_table() {
        let mut st = StateTable::new();
        let e = id("cvar:dut:mode");
        st.register(e.clone(), None).unwrap();
        // A → B → A: two distinct names, ordinals by first appearance (A=0, B=1),
        // and the repeat of A reuses ordinal 0.
        for (t, name) in [(1_000, "IDLE"), (2_000, "RUN"), (3_000, "IDLE")] {
            st.set_time(t);
            st.record(&e, Value::Enum(name.into())).unwrap();
        }
        let decoded = decode(&dump(&st, None));
        let se = decoded.iter().find(|d| d.id == "cvar:dut:mode").unwrap();
        assert_eq!(se.dtype, DT_ENUM);
        assert_eq!(
            se.values,
            vec![DecodedValue::Enum(0), DecodedValue::Enum(1), DecodedValue::Enum(0)]
        );
        assert_eq!(
            se.enum_table,
            vec![(0, "IDLE".to_string()), (1, "RUN".to_string())]
        );
    }

    #[test]
    fn bytes_and_never_recorded_signals_are_skipped() {
        let mut st = StateTable::new();
        st.register(id("spi:m:cs:rx"), None).unwrap(); // Bytes once recorded
        st.register(id("cvar:dut:never"), None).unwrap(); // registered, never recorded
        st.register(id("cvar:dut:kept"), None).unwrap();
        st.set_time(1_000);
        st.force_record(&id("spi:m:cs:rx"), Value::Bytes(vec![1, 2])).unwrap();
        st.record(&id("cvar:dut:kept"), Value::U32(7)).unwrap();

        let decoded = decode(&dump(&st, None));
        let ids: Vec<&str> = decoded.iter().map(|d| d.id.as_str()).collect();
        assert_eq!(ids, vec!["cvar:dut:kept"]);
    }

    #[test]
    fn prefix_filter_keeps_only_matching_signals() {
        let mut st = StateTable::new();
        for name in ["vsig:motor:a", "vsig:dial:b", "cvar:dut:c"] {
            let s = id(name);
            st.register(s.clone(), None).unwrap();
            st.set_time(1_000);
            st.record(&s, Value::U32(1)).unwrap();
        }
        let decoded = decode(&dump(&st, Some(&["vsig:motor:", "cvar:"])));
        let mut ids: Vec<&str> = decoded.iter().map(|d| d.id.as_str()).collect();
        ids.sort();
        assert_eq!(ids, vec!["cvar:dut:c", "vsig:motor:a"]);
    }
}
