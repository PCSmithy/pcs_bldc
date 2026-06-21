//! Raw FFI to the firmware shared library.
//!
//! Loads `libpcs_bldc_fw.dll` and exposes the D2 control ABI
//! (`sil_fw_start` / `sil_fw_advance_tick` / `sil_fw_shutdown`) plus white-box
//! access to exported globals. This is the in-process boundary from
//! `docs/sil/ffi-boundary.md`: the framework *drives* the firmware through the
//! three ABI calls and *reads/writes* its state directly in memory.
//!
//! Today symbol access is via the DLL export table (exported globals only). The
//! DWARF symbol/type map — which reaches every `static`, not just exports —
//! lands here next and becomes the firmware backing of the State Table.

use libloading::{Library, Symbol};
use std::path::Path;

/// A loaded firmware instance (one per process — see ffi-boundary.md §1).
pub struct Firmware {
    lib: Library,
}

impl Firmware {
    /// Load the firmware shared library.
    pub fn load(path: &Path) -> Result<Self, libloading::Error> {
        // SAFETY: loading a trusted, project-built artifact. Its initializers
        // are the C runtime's; no Rust invariants depend on them.
        let lib = unsafe { Library::new(path)? };
        Ok(Self { lib })
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

    /// White-box read of an exported `uint32_t` global by name (e.g.
    /// `b"sim_task1msRuns\0"`). Read between ticks while the firmware is
    /// quiescent, so a plain (non-atomic) read is race-free (ffi-boundary.md §5).
    pub fn read_u32(&self, name: &[u8]) -> u32 {
        // SAFETY: the symbol names a `uint32_t` global. libloading hands back
        // the symbol *address* reinterpreted as the requested (pointer-sized)
        // type, so a data global is `Symbol<*mut u32>` — `*sym` is the
        // variable's address, `**sym` reads it. The firmware is quiescent, so
        // the read is race-free.
        unsafe {
            let sym: Symbol<*mut u32> = self.lib.get(name).expect("global symbol");
            **sym
        }
    }
}
