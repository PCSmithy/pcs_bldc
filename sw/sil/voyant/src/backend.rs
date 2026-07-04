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

/// A firmware execution backend: load, drive, and introspect one firmware
/// instance. This is the framework's single narrow seam onto the
/// firmware-under-test (architecture.md §3.2) — lifecycle
/// (`start`/`advance_tick`/`shutdown`) plus white-box `cvar` read/write by path.
/// [`Firmware`] (native shared lib + DWARF) is the first impl; another backend
/// (e.g. ARM emulation) could implement the same trait without disturbing the
/// engine, models, State Table, or run modes.
///
/// All methods take `&self`: a backend mutates *external* state (the firmware's
/// own memory / execution), not the Rust handle, so it needs no `&mut`.
/// Construction (loading the artifact) is backend-specific and stays off the
/// trait — see [`Firmware::load`].
pub trait Backend {
    /// Bring the firmware up: run HW/app init, create tasks, and run the
    /// scheduler to first quiescence. Returns false on init/task-creation
    /// failure.
    fn start(&self) -> bool;

    /// Advance one sim tick (run the firmware to its next quiescence).
    fn advance_tick(&self);

    /// Tear the firmware down.
    fn shutdown(&self);

    /// Sample a firmware `static` by path into a logical [`Value`] — the read
    /// side of the State Table's `cvar` backing.
    fn read_cvar(&self, path: &str) -> Value;

    /// Write a logical [`Value`] into a firmware `static` by path — white-box
    /// injection (the write side of the `cvar` backing).
    fn write_cvar(&self, path: &str, v: &Value);
}

/// A loaded firmware instance (one per process — see ffi-boundary.md §1).
pub struct Firmware {
    lib: Library,
    dwarf: DwarfMap,
    /// runtime_addr - link_addr, applied to every DWARF address (ASLR slide).
    slide: u64,
}

/// An exported firmware global used to anchor the ASLR slide (its runtime
/// address vs its DWARF link address). Only the symbol's address is used, never
/// its value, so any exported data global present in DWARF works; this one is a
/// stable const config always present in the image.
const ANCHOR: &str = "HW_ADC_config";

impl Firmware {
    /// Load the firmware shared library and its DWARF.
    pub fn load(path: &Path) -> Result<Self, Box<dyn Error>> {
        let dwarf = DwarfMap::from_lib_path(path)?;

        // SAFETY: loading a trusted, project-built artifact.
        let lib = unsafe { Library::new(path)? };

        let link_anchor = dwarf
            .var_addr(ANCHOR)
            .ok_or("anchor symbol missing from DWARF")?;
        let runtime_anchor = {
            // SAFETY: ANCHOR names an exported data global; we only take the
            // symbol's runtime address (never dereference it), so its type is
            // immaterial — `*mut u32` is just a placeholder pointer type.
            let sym: Symbol<*mut u32> = unsafe { lib.get(ANCHOR.as_bytes())? };
            *sym as u64
        };
        let slide = runtime_anchor.wrapping_sub(link_anchor);

        Ok(Self { lib, dwarf, slide })
    }

    /// White-box read of an **exported** `uint32_t` global by name (export
    /// table). For arbitrary statics use [`Backend::read_cvar`].
    pub fn read_u32(&self, name: &[u8]) -> u32 {
        // SAFETY: a data global is `Symbol<*mut u32>`; `**sym` reads it.
        unsafe {
            let sym: Symbol<*mut u32> = self.lib.get(name).expect("global symbol");
            **sym
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

impl Backend for Firmware {
    /// Control ABI: HW init + create tasks + run the scheduler to first
    /// quiescence. Returns false on init/task-creation failure.
    fn start(&self) -> bool {
        // SAFETY: signature matches `bool sil_fw_start(void)`.
        unsafe {
            let f: Symbol<unsafe extern "C" fn() -> bool> =
                self.lib.get(b"sil_fw_start\0").expect("sil_fw_start");
            f()
        }
    }

    /// Control ABI: advance one sim tick (run firmware to next quiescence).
    fn advance_tick(&self) {
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
    fn shutdown(&self) {
        // SAFETY: signature matches `void sil_fw_shutdown(void)`.
        unsafe {
            let f: Symbol<unsafe extern "C" fn()> =
                self.lib.get(b"sil_fw_shutdown\0").expect("sil_fw_shutdown");
            f()
        }
    }

    /// Sample a firmware `static` by DWARF path into a logical [`Value`]
    /// (the cvar sample-resolver). Scalar widths are coerced; an enum field
    /// reads as its symbolic [`Value::Enum`] name (or `<n>` for an unknown
    /// enumerator, e.g. a bitwise combination).
    fn read_cvar(&self, path: &str) -> Value {
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
    fn write_cvar(&self, path: &str, v: &Value) {
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::HashMap;

    /// A pure-Rust [`Backend`] with no DLL, used to prove the trait is
    /// object-safe and usable behind `dyn`/`Box` without touching real firmware.
    #[derive(Default)]
    struct MockBackend {
        started: RefCell<bool>,
        ticks: RefCell<u64>,
        cvars: RefCell<HashMap<String, Value>>,
    }

    impl Backend for MockBackend {
        fn start(&self) -> bool {
            *self.started.borrow_mut() = true;
            true
        }
        fn advance_tick(&self) {
            *self.ticks.borrow_mut() += 1;
        }
        fn shutdown(&self) {
            *self.started.borrow_mut() = false;
        }
        fn read_cvar(&self, path: &str) -> Value {
            self.cvars
                .borrow()
                .get(path)
                .cloned()
                .unwrap_or(Value::U32(0))
        }
        fn write_cvar(&self, path: &str, v: &Value) {
            self.cvars.borrow_mut().insert(path.to_string(), v.clone());
        }
    }

    #[test]
    fn backend_is_object_safe_and_usable_via_dyn() {
        let be: Box<dyn Backend> = Box::new(MockBackend::default());
        assert!(be.start());
        be.advance_tick();
        be.advance_tick();
        be.write_cvar("x", &Value::U32(42));
        assert_eq!(be.read_cvar("x"), Value::U32(42));
        assert_eq!(be.read_cvar("unset"), Value::U32(0));
        be.shutdown();
    }

    #[test]
    fn backend_usable_behind_ref_dyn() {
        // Prove `&dyn Backend` flows through a generic-free function boundary,
        // the shape the engine loop uses.
        fn drive(be: &dyn Backend) -> u32 {
            be.write_cvar("n", &Value::U32(7));
            match be.read_cvar("n") {
                Value::U32(x) => x,
                _ => 0,
            }
        }
        let be = MockBackend::default();
        assert_eq!(drive(&be), 7);
    }
}
