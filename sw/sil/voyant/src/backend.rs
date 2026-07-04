//! The native firmware backend: load the firmware shared library, drive it over
//! the control ABI, and read/write its state in-process. It is also the **cvar
//! sample-resolver** — it reads a firmware `static` (any width) and coerces it
//! into the logical [`Value`], and writes a [`Value`] back into firmware memory.
//!
//! This is the in-process boundary from `docs/sil/ffi-boundary.md`, and the
//! only firmware-coupled (unsafe / DWARF) part of the framework; the State Table
//! itself is pure data, fed by this resolver.

use crate::dwarf::{DwarfMap, Leaf, Scalar};
use crate::signal::Value;
use libloading::{Library, Symbol};
use std::error::Error;
use std::path::Path;

/// A loaded firmware instance (one per process — see ffi-boundary.md §1).
pub struct Firmware {
    lib: Library,
    dwarf: DwarfMap,
    /// runtime_addr - link_addr, applied to every DWARF address (ASLR slide).
    slide: u64,
}

/// The exported global used to anchor the ASLR slide.
const ANCHOR: &str = "sim_task1msRuns";

impl Firmware {
    /// Load the firmware shared library and its DWARF.
    pub fn load(path: &Path) -> Result<Self, Box<dyn Error>> {
        let bytes = std::fs::read(path)?;
        let dwarf = DwarfMap::parse(&bytes)?;

        // SAFETY: loading a trusted, project-built artifact.
        let lib = unsafe { Library::new(path)? };

        let link_anchor = dwarf
            .var_addr(ANCHOR)
            .ok_or("anchor symbol missing from DWARF")?;
        let runtime_anchor = {
            // SAFETY: ANCHOR names an exported `uint32_t`; the symbol address is
            // its runtime address.
            let sym: Symbol<*mut u32> = unsafe { lib.get(ANCHOR.as_bytes())? };
            *sym as u64
        };
        let slide = runtime_anchor.wrapping_sub(link_anchor);

        Ok(Self { lib, dwarf, slide })
    }

    /// Control ABI: HW init + create tasks + run the scheduler to first
    /// quiescence. Returns false on init/task-creation failure.
    pub fn start(&self) -> bool {
        // SAFETY: signature matches `bool sil_fw_start(void)`.
        unsafe {
            let f: Symbol<unsafe extern "C" fn() -> bool> =
                self.lib.get(b"sil_fw_start\0").expect("sil_fw_start");
            f()
        }
    }

    /// Control ABI: advance one sim tick (run firmware to next quiescence).
    pub fn advance_tick(&self) {
        // SAFETY: signature matches `void sil_fw_advance_tick(void)`.
        unsafe {
            let f: Symbol<unsafe extern "C" fn()> = self
                .lib
                .get(b"sil_fw_advance_tick\0")
                .expect("sil_fw_advance_tick");
            f()
        }
    }

    /// Control ABI: tear down the scheduler.
    pub fn shutdown(&self) {
        // SAFETY: signature matches `void sil_fw_shutdown(void)`.
        unsafe {
            let f: Symbol<unsafe extern "C" fn()> =
                self.lib.get(b"sil_fw_shutdown\0").expect("sil_fw_shutdown");
            f()
        }
    }

    /// White-box read of an **exported** `uint32_t` global by name (export
    /// table). For arbitrary statics use [`read_cvar`](Self::read_cvar).
    pub fn read_u32(&self, name: &[u8]) -> u32 {
        // SAFETY: a data global is `Symbol<*mut u32>`; `**sym` reads it.
        unsafe {
            let sym: Symbol<*mut u32> = self.lib.get(name).expect("global symbol");
            **sym
        }
    }

    /// Sample a firmware `static` by DWARF path into a logical [`Value`]
    /// (the cvar sample-resolver). Scalar widths are coerced; an enum field
    /// reads as its symbolic [`Value::Enum`] name (or `<n>` for an unknown
    /// enumerator, e.g. a bitwise combination).
    pub fn read_cvar(&self, path: &str) -> Value {
        let (p, leaf) = self.resolve(path);
        // SAFETY: valid firmware address (DWARF + slide); firmware quiescent.
        unsafe {
            match leaf {
                Leaf::Scalar(kind) => scalar_to_value(p, kind),
                Leaf::Enum(off) => {
                    let n = read_uint(p, self.dwarf.enum_size(off).unwrap()) as i64;
                    match self.dwarf.enum_name(off, n) {
                        Some(name) => Value::Enum(name.to_string()),
                        None => Value::Enum(format!("<{n}>")),
                    }
                }
            }
        }
    }

    /// Write a logical [`Value`] into a firmware `static` by DWARF path. Scalars
    /// coerce to the field's width; an enum accepts [`Value::Enum`] (name → its
    /// value) or a raw `U32`/`I32`. Panics on an incompatible variant.
    pub fn write_cvar(&self, path: &str, v: &Value) {
        let (p, leaf) = self.resolve(path);
        // SAFETY: valid firmware address of the field's size; firmware quiescent.
        unsafe {
            match leaf {
                Leaf::Scalar(kind) => value_to_scalar(p, kind, v),
                Leaf::Enum(off) => {
                    let n = match v {
                        Value::Enum(name) => self
                            .dwarf
                            .enum_value(off, name)
                            .unwrap_or_else(|| panic!("unknown enumerator {name:?} for {path}")),
                        Value::U32(x) => *x as i64,
                        Value::I32(x) => *x as i64,
                        other => panic!("cannot write {other:?} to enum {path}"),
                    };
                    write_uint(p, self.dwarf.enum_size(off).unwrap(), n as u64);
                }
            }
        }
    }

    fn resolve(&self, path: &str) -> (*mut u8, Leaf) {
        let (link, leaf) = self
            .dwarf
            .resolve(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        (link.wrapping_add(self.slide) as *mut u8, leaf)
    }
}

/// Read an unsigned integer of `size` bytes.
/// SAFETY: `p` points at `size` readable bytes.
unsafe fn read_uint(p: *const u8, size: u64) -> u64 {
    match size {
        1 => p.read_unaligned() as u64,
        2 => (p as *const u16).read_unaligned() as u64,
        4 => (p as *const u32).read_unaligned() as u64,
        8 => (p as *const u64).read_unaligned(),
        _ => panic!("unsupported enum size {size}"),
    }
}

/// Write an integer's low `size` bytes.
/// SAFETY: `p` points at `size` writable bytes.
unsafe fn write_uint(p: *mut u8, size: u64, v: u64) {
    match size {
        1 => p.write_unaligned(v as u8),
        2 => (p as *mut u16).write_unaligned(v as u16),
        4 => (p as *mut u32).write_unaligned(v as u32),
        8 => (p as *mut u64).write_unaligned(v),
        _ => panic!("unsupported enum size {size}"),
    }
}

/// Read a firmware scalar and coerce it to the logical [`Value`] set: signed
/// widths → `I32`, unsigned → `U32`/`U64`, plus float/bool. (`I64` narrows to
/// `I32` for now — rare in firmware; enums read as their numeric `U32`. Both
/// are noted follow-ups: add `I64` / DWARF enum-name resolution when needed.)
///
/// SAFETY: `p` points at a readable value of `kind`'s size; firmware quiescent.
unsafe fn scalar_to_value(p: *const u8, kind: Scalar) -> Value {
    match kind {
        Scalar::U8 => Value::U32(p.read_unaligned() as u32),
        Scalar::U16 => Value::U32((p as *const u16).read_unaligned() as u32),
        Scalar::U32 => Value::U32((p as *const u32).read_unaligned()),
        Scalar::U64 => Value::U64((p as *const u64).read_unaligned()),
        Scalar::I8 => Value::I32((p as *const i8).read_unaligned() as i32),
        Scalar::I16 => Value::I32((p as *const i16).read_unaligned() as i32),
        Scalar::I32 => Value::I32((p as *const i32).read_unaligned()),
        Scalar::I64 => Value::I32((p as *const i64).read_unaligned() as i32),
        Scalar::F32 => Value::F32((p as *const f32).read_unaligned()),
        Scalar::F64 => Value::F64((p as *const f64).read_unaligned()),
        Scalar::Bool => Value::Bool(p.read_unaligned() != 0),
    }
}

/// Coerce a logical [`Value`] back into a firmware scalar of `kind` and write.
///
/// SAFETY: `p` points at a writable value of `kind`'s size; firmware quiescent.
unsafe fn value_to_scalar(p: *mut u8, kind: Scalar, v: &Value) {
    match (kind, v) {
        (Scalar::U8, Value::U32(x)) => p.write_unaligned(*x as u8),
        (Scalar::U16, Value::U32(x)) => (p as *mut u16).write_unaligned(*x as u16),
        (Scalar::U32, Value::U32(x)) => (p as *mut u32).write_unaligned(*x),
        (Scalar::U64, Value::U64(x)) => (p as *mut u64).write_unaligned(*x),
        (Scalar::I8, Value::I32(x)) => (p as *mut i8).write_unaligned(*x as i8),
        (Scalar::I16, Value::I32(x)) => (p as *mut i16).write_unaligned(*x as i16),
        (Scalar::I32, Value::I32(x)) => (p as *mut i32).write_unaligned(*x),
        (Scalar::I64, Value::I32(x)) => (p as *mut i64).write_unaligned(*x as i64),
        (Scalar::F32, Value::F32(x)) => (p as *mut f32).write_unaligned(*x),
        (Scalar::F64, Value::F64(x)) => (p as *mut f64).write_unaligned(*x),
        (Scalar::Bool, Value::Bool(x)) => p.write_unaligned(*x as u8),
        (k, val) => panic!("cvar write type mismatch: firmware {k:?} vs value {val:?}"),
    }
}
