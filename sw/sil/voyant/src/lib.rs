//! # voyant — clairvoyant software-in-the-loop
//!
//! A generic, firmware-agnostic framework for running cross-compiled embedded
//! firmware in a deterministic virtual world, with total white-box visibility
//! into its state. A project (e.g. `pcs_bldc_sil`) instantiates voyant by
//! implementing its trait seams and supplying its firmware, models, and routes;
//! nothing here is specific to any one board.
//!
//! Today voyant provides the **native firmware backend** ([`Firmware`]): load
//! the firmware shared library, drive it over the control ABI
//! (`sil_fw_start` / `sil_fw_advance_tick` / `sil_fw_shutdown`), and read/write
//! any of its state — exported globals via the export table, and **any**
//! `static` (typed, incl. array elements + struct members) via DWARF. This is
//! the in-process boundary from `docs/sil/ffi-boundary.md`. The State Table,
//! Route Table, sim clock, historian, and run modes build on it.

mod dwarf;
pub use dwarf::{DwarfMap, Scalar};

use libloading::{Library, Symbol};
use std::error::Error;
use std::fmt;
use std::path::Path;

/// A typed scalar firmware value (variant chosen from the DWARF leaf type).
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
    fn scalar(self) -> Scalar {
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
            let f: Symbol<unsafe extern "C" fn()> =
                self.lib.get(b"sil_fw_advance_tick\0").expect("sil_fw_advance_tick");
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
    /// table). For arbitrary statics use [`read`](Self::read).
    pub fn read_u32(&self, name: &[u8]) -> u32 {
        // SAFETY: a data global is `Symbol<*mut u32>` — `*sym` is its address,
        // `**sym` reads it. Firmware quiescent → race-free.
        unsafe {
            let sym: Symbol<*mut u32> = self.lib.get(name).expect("global symbol");
            **sym
        }
    }

    /// White-box typed read of **any** firmware value by DWARF path
    /// (`var.member`, `arr[i]`, nested), including non-exported statics. Read
    /// between ticks while the firmware is quiescent.
    pub fn read(&self, path: &str) -> Value {
        let (p, kind) = self.live_ptr(path);
        // SAFETY: `p` is a valid firmware address (DWARF + slide); firmware
        // quiescent. Unaligned read is always sound.
        unsafe {
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
    }

    /// White-box typed write of any firmware value by DWARF path. The value's
    /// type must match the DWARF leaf type. Write between ticks while the
    /// firmware is quiescent.
    pub fn write(&self, path: &str, v: Value) {
        let (p, kind) = self.live_ptr(path);
        assert_eq!(
            v.scalar(),
            kind,
            "write type mismatch for {path}: value is {:?}, firmware field is {:?}",
            v.scalar(),
            kind
        );
        let p = p as *mut u8;
        // SAFETY: `p` is a valid firmware address of the matching size; firmware
        // quiescent.
        unsafe {
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
    }

    /// Resolve a DWARF path to its live in-process address + scalar kind.
    fn live_ptr(&self, path: &str) -> (*const u8, Scalar) {
        let (link_addr, kind) = self
            .dwarf
            .resolve(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        (link_addr.wrapping_add(self.slide) as *const u8, kind)
    }
}
