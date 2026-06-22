//! Raw FFI to the firmware shared library.
//!
//! Loads `libpcs_bldc_fw.dll` and exposes the D2 control ABI
//! (`sil_fw_start` / `sil_fw_advance_tick` / `sil_fw_shutdown`) plus white-box
//! access to firmware state. This is the in-process boundary from
//! `docs/sil/ffi-boundary.md`: the framework *drives* the firmware through the
//! three ABI calls and *reads/writes* its state directly in memory.
//!
//! Two layers of state access:
//!   - exported globals via the DLL export table (`read_u32`), and
//!   - **any** `static` via DWARF (`read_path_u32`) — the State Table backing.

mod dwarf;
pub use dwarf::DwarfMap;

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

/// The exported global used to anchor the ASLR slide: its runtime address
/// (export table) vs its DWARF link address pins runtime = link + slide.
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
    /// table). For non-exported statics use [`read_path_u32`](Self::read_path_u32).
    pub fn read_u32(&self, name: &[u8]) -> u32 {
        // SAFETY: a data global is `Symbol<*mut u32>` — `*sym` is its address,
        // `**sym` reads it. The firmware is quiescent → race-free.
        unsafe {
            let sym: Symbol<*mut u32> = self.lib.get(name).expect("global symbol");
            **sym
        }
    }

    /// White-box read of **any** `uint32_t` by DWARF path (`var.member...`),
    /// including non-exported statics. Resolves the link address via DWARF,
    /// applies the ASLR slide, and reads in-process. Read between ticks while
    /// the firmware is quiescent.
    pub fn read_path_u32(&self, path: &str) -> u32 {
        let (link_addr, _size) = self
            .dwarf
            .resolve(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        let live = link_addr.wrapping_add(self.slide);
        // SAFETY: `live` is a valid firmware address (DWARF + slide); the
        // firmware is quiescent.
        unsafe { *(live as *const u32) }
    }
}
