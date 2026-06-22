//! The native firmware backend: load the firmware shared library, drive it over
//! the control ABI, and read/write its state in-process.
//!
//! This is the in-process boundary from `docs/sil/ffi-boundary.md`. It is
//! generic over any C firmware built as a host shared lib with a `sil_fw_*`
//! control ABI + DWARF (the symbol/anchor names are the only firmware-specific
//! bits, and are configurable later when this becomes the `Backend` trait impl).

use crate::dwarf::DwarfMap;
use crate::value::{self, Scalar, Value};
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
    /// table). For arbitrary statics use [`read`](Self::read) / the State Table.
    pub fn read_u32(&self, name: &[u8]) -> u32 {
        // SAFETY: a data global is `Symbol<*mut u32>`; `**sym` reads it.
        unsafe {
            let sym: Symbol<*mut u32> = self.lib.get(name).expect("global symbol");
            **sym
        }
    }

    /// Resolve a DWARF path (`var.member`, `arr[i]`, nested) to its live
    /// in-process address + scalar kind. The address is valid while `self`
    /// (and thus the loaded DLL) lives. Used by the State Table to back `cvar`
    /// signals.
    pub fn resolve_addr(&self, path: &str) -> Option<(*mut u8, Scalar)> {
        let (link, kind) = self.dwarf.resolve(path)?;
        Some((link.wrapping_add(self.slide) as *mut u8, kind))
    }

    /// White-box typed read of any firmware value by DWARF path.
    pub fn read(&self, path: &str) -> Value {
        let (p, kind) = self
            .resolve_addr(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        // SAFETY: valid firmware address (DWARF + slide); firmware quiescent.
        unsafe { value::read_scalar(p, kind) }
    }

    /// White-box typed write of any firmware value by DWARF path (type-checked).
    pub fn write(&self, path: &str, v: Value) {
        let (p, kind) = self
            .resolve_addr(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        // SAFETY: valid firmware address of matching size; firmware quiescent.
        unsafe { value::write_scalar(p, kind, v) }
    }
}
