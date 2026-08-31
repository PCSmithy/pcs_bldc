//! The [`Sil`] harness: **the simulation itself**. It owns a fresh [`Engine`] and
//! derefs to it, so the whole engine API (`add_member`, `link_duplex`, `step`, …) is
//! called directly on a `Sil`. [`Sil::new`] is a zero-firmware world on the default
//! grid; [`Sil::options`] builds one on a caller-chosen grid. Firmware is
//! loaded per instance with [`load_firmware`](Sil::load_firmware), each call booting
//! one image copied to its own temp path. Drop dumps a trace, shuts firmwares down,
//! unloads them, and deletes the copies.

use crate::{dll_path, trace, TICK_US};
use std::ops::{Deref, DerefMut};
use std::path::{Path, PathBuf};
use std::rc::{Rc, Weak};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, MutexGuard};
use voyant::{
    Engine, Firmware, FirmwareMember, StateTable, StateTableConfig, Value,
    DEFAULT_SWEEP_PERIOD_US,
};

/// Process-global lock, taken at a world's first [`load_firmware`](Sil::load_firmware)
/// and held until drop, so vanilla (threaded) `cargo test` serializes access to the
/// loadable firmware image. Poison is recovered
/// ([`into_inner`](std::sync::PoisonError::into_inner)) so one panicking test releases
/// the lock without cascade-failing the rest. Under cargo-nextest each test runs in
/// its own process, so the lock is uncontended. A model-only world never takes it.
static WORLD_LOCK: Mutex<()> = Mutex::new(());

/// Distinguishes the temp copies within a process (see [`unique_temp_copy`]).
static COPY_COUNTER: AtomicU64 = AtomicU64::new(0);

/// How a world is built. [`Sil::new`] is `SilOptions::default()`; a scenario needing
/// a finer grid, an exact-value table, or a different mirror cadence starts from
/// [`Sil::options`] and sets only what it cares about:
///
/// ```ignore
/// let mut sim = Sil::options().grid_us(50).build();
/// ```
#[derive(Clone)]
pub struct SilOptions {
    grid_us: u64,
    state: StateTableConfig,
    sweep_period_us: u64,
}

impl Default for SilOptions {
    fn default() -> Self {
        Self {
            grid_us: TICK_US,
            state: StateTableConfig::default(),
            sweep_period_us: DEFAULT_SWEEP_PERIOD_US,
        }
    }
}

impl SilOptions {
    /// The engine grid: µs of sim time per step. Every interrupt dispatches on the
    /// step its due time falls in, so a finer grid tightens that quantization.
    pub fn grid_us(mut self, us: u64) -> Self {
        self.grid_us = us;
        self
    }

    /// The State Table config — `epsilon: 0.0` for checks asserting exact values the
    /// default change deadband would blur.
    pub fn state_config(mut self, config: StateTableConfig) -> Self {
        self.state = config;
        self
    }

    /// The cvar mirror cadence every firmware this world loads runs on: the
    /// whole-namespace sweep happens at most once per `us` of sim time (0 = every
    /// dispatching step). Bounds historian latency; drops nothing.
    pub fn sweep_period_us(mut self, us: u64) -> Self {
        self.sweep_period_us = us;
        self
    }

    /// Build the world.
    pub fn build(self) -> Sil {
        Sil {
            engine: Some(Engine::with_state(
                self.grid_us,
                StateTable::with_config(self.state),
            )),
            firmwares: Vec::new(),
            temp_paths: Vec::new(),
            guard: None,
            sweep_period_us: self.sweep_period_us,
        }
    }
}

/// The simulation: a fresh [`Engine`] plus the firmware images loaded into it. Build
/// one per `#[test]`, [`load_firmware`](Self::load_firmware) any instances it needs,
/// [`add_member`](Engine::add_member) them, and [`step`](Engine::step). Derefs to the
/// [`Engine`], so the engine API is direct on the value. Dropping it dumps a trace,
/// shuts every firmware down, unloads the libraries, and removes the temp copies.
pub struct Sil {
    /// `Some` for a live world; taken to `None` during drop so the engine (and its
    /// member `Rc`s) release before the temp copies are deleted.
    engine: Option<Engine>,
    /// One `Weak` per loaded firmware, in load order. The member owns the **sole**
    /// strong `Rc` (so a member can reload its own image from reset — a strong clone
    /// here would break that sole-ownership assert); this weak drives `fw()` and
    /// shutdown-on-drop, and reads `None` once a member has reloaded or the engine has
    /// dropped it.
    firmwares: Vec<Weak<Firmware>>,
    /// The temp DLL copies backing `firmwares`, in load order — deleted after unload.
    temp_paths: Vec<PathBuf>,
    /// Held from the first firmware load until drop (see [`WORLD_LOCK`]); `None` in a
    /// model-only world.
    guard: Option<MutexGuard<'static, ()>>,
    /// The mirror cadence applied to every firmware this world loads.
    sweep_period_us: u64,
}

impl Sil {
    /// A fresh, zero-firmware world on the default grid: a new [`Engine`] and nothing
    /// else. Takes no lock and loads no DLL — model-only scenarios are first-class.
    /// Add firmware with [`load_firmware`](Self::load_firmware).
    pub fn new() -> Self {
        Self::options().build()
    }

    /// A zero-firmware world whose State Table runs `config` — `epsilon: 0.0` for checks
    /// asserting exact values the default 1e-3 change deadband would blur.
    pub fn with_config(config: StateTableConfig) -> Self {
        Self::options().state_config(config).build()
    }

    /// The build options, for a world that wants a non-default grid / table / mirror
    /// cadence: `Sil::options().grid_us(50).build()`.
    pub fn options() -> SilOptions {
        SilOptions::default()
    }

    /// One signal as an `f64`, or `NaN` when it is unregistered or unset.
    pub fn read_f64(&self, id: &str) -> f64 {
        self.read(id)
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_f64)
            .unwrap_or(f64::NAN)
    }

    /// One signal as a `u64`, or 0 when it is unregistered or unset.
    pub fn read_u64(&self, id: &str) -> u64 {
        self.read(id)
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_u64)
            .unwrap_or(0)
    }

    /// One signal as a `bool` — true only for a live [`Value::Bool(true)`](Value::Bool).
    pub fn read_bool(&self, id: &str) -> bool {
        matches!(self.read(id).ok().flatten(), Some(Value::Bool(true)))
    }

    /// Load and boot one firmware instance from the build-tree default
    /// ([`dll_path`]), named `source_name` for its `cvar:<source>:…` namespace.
    /// Returns the bound [`FirmwareMember`] — configure it (e.g.
    /// [`register_cvar_in_state_table`]), then [`add_member`](Engine::add_member) it.
    ///
    /// [`register_cvar_in_state_table`]: voyant::FirmwareMember::register_cvar_in_state_table
    pub fn load_firmware(&mut self, source_name: &str) -> FirmwareMember {
        let path = dll_path();
        self.load_firmware_from(source_name, &path)
    }

    /// [`load_firmware`](Self::load_firmware) from an explicit library path, taking the
    /// process lock on this world's first load. The image is copied to a unique temp file
    /// — `LoadLibrary` on one path aliases the module's statics, so the copy is what gives
    /// each instance its own memory — and that copy is also the member's reload recipe, so
    /// a disable/re-enable reboots from reset. Panics if the DLL is missing (build it:
    /// `tools/run_sil.sh`), the copy or load fails, or `start()` returns false.
    pub fn load_firmware_from(&mut self, source_name: &str, path: &Path) -> FirmwareMember {
        if self.guard.is_none() {
            self.guard = Some(WORLD_LOCK.lock().unwrap_or_else(|p| p.into_inner()));
        }
        assert!(
            path.exists(),
            "firmware DLL not found at {}; build it first: tools/run_sil.sh",
            path.display()
        );
        let copy = unique_temp_copy(path).unwrap_or_else(|e| {
            panic!(
                "copy firmware {} to a temp file failed: {e}",
                path.display()
            )
        });
        let fw = Rc::new(
            Firmware::load(&copy)
                .unwrap_or_else(|e| panic!("failed to load firmware {}: {e}", copy.display())),
        );
        assert!(fw.start(), "sil_fw_start() returned false");
        self.firmwares.push(Rc::downgrade(&fw));
        self.temp_paths.push(copy.clone());
        let mut member = FirmwareMember::new(source_name, fw);
        // Reboot-from-reset recipe: re-enabling this member reloads the same temp copy.
        member.set_reload_path(&copy);
        member.set_sweep_period_us(self.sweep_period_us);
        member
    }

    /// The most-recently loaded firmware handle, for checks that read a `static` out of
    /// firmware MEMORY directly. Panics in a zero-firmware world, or if the tracked handle
    /// is stale (the member reloaded its image).
    pub fn fw(&self) -> Rc<Firmware> {
        self.firmwares
            .last()
            .expect("no firmware loaded")
            .upgrade()
            .expect("firmware handle is stale (member reloaded or unloaded its image)")
    }

    /// Step the engine for `ms` milliseconds of sim time (at this world's grid),
    /// panicking on a step error.
    pub fn run_for_ms(&mut self, ms: u64) {
        let ticks = (ms * 1_000) / self.grid_us();
        for _ in 0..ticks {
            self.step().expect("engine step");
        }
    }
}

impl Default for Sil {
    fn default() -> Self {
        Self::new()
    }
}

impl Deref for Sil {
    type Target = Engine;
    fn deref(&self) -> &Engine {
        self.engine.as_ref().expect("engine present")
    }
}

impl DerefMut for Sil {
    fn deref_mut(&mut self) -> &mut Engine {
        self.engine.as_mut().expect("engine present")
    }
}

/// Acquire the process-global world lock directly — for reload experiments (the
/// lifecycle spike) that drive raw [`Firmware`] cycles outside a [`Sil`]. Hold it on
/// one thread for the duration; every world's first firmware load blocks until it
/// drops, so no second firmware load overlaps.
pub fn lock_world() -> MutexGuard<'static, ()> {
    WORLD_LOCK.lock().unwrap_or_else(|p| p.into_inner())
}

/// Copy `src` to a per-process-unique file in the system temp dir, preserving the
/// original file name (and extension, which `LoadLibrary` needs) behind a unique
/// prefix. A sibling `<src>.dSYM` bundle rides along — on macOS the DWARF lives
/// there, not in the dylib. Returns the copy's path.
fn unique_temp_copy(src: &Path) -> std::io::Result<PathBuf> {
    let stem = src
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "fw".into());
    let n = COPY_COUNTER.fetch_add(1, Ordering::Relaxed);
    let dst = std::env::temp_dir().join(format!("pcs_sil_{}_{}_{}", std::process::id(), n, stem));
    std::fs::copy(src, &dst)?;
    copy_dsym_sibling(src, &dst)?;
    Ok(dst)
}

/// The `.dSYM` bundle path sitting next to `image` (`<image>.dSYM`).
fn dsym_bundle(image: &Path) -> PathBuf {
    let mut b = image.as_os_str().to_owned();
    b.push(".dSYM");
    PathBuf::from(b)
}

/// Give the temp copy its own `.dSYM`: the DWARF reader opens
/// `<image>.dSYM/Contents/Resources/DWARF/<image filename>`, so the inner file
/// must carry the copy's name, not the original's. A no-op when `src` has no
/// bundle (Windows/Linux, or an image with DWARF in-file).
fn copy_dsym_sibling(src: &Path, dst: &Path) -> std::io::Result<()> {
    let (Some(src_name), Some(dst_name)) = (src.file_name(), dst.file_name()) else {
        return Ok(());
    };
    let src_dwarf = dsym_bundle(src)
        .join("Contents")
        .join("Resources")
        .join("DWARF")
        .join(src_name);
    if !src_dwarf.is_file() {
        return Ok(());
    }
    let dst_dir = dsym_bundle(dst)
        .join("Contents")
        .join("Resources")
        .join("DWARF");
    std::fs::create_dir_all(&dst_dir)?;
    std::fs::copy(&src_dwarf, dst_dir.join(dst_name))?;
    Ok(())
}

impl Drop for Sil {
    fn drop(&mut self) {
        // Per-test trace (no-op unless PCS_SIL_TRACE_DIR is set), named after the
        // running test's thread — cargo test names each test thread after its fn.
        let name = std::thread::current().name().unwrap_or("trace").to_string();
        if let Some(eng) = self.engine.as_ref() {
            trace::maybe_dump(eng, &name);
        }
        // Shut every still-live firmware down in reverse load order (LIFO teardown).
        // A stale weak — a member that reloaded its image, or one already dropped —
        // upgrades to `None` and is skipped; the member owns the strong `Rc`, so the
        // upgrade succeeds while the engine is alive.
        for weak in self.firmwares.iter().rev() {
            if let Some(fw) = weak.upgrade() {
                fw.shutdown();
            }
        }
        // Drop the engine, releasing each member's firmware `Rc` — the last clone's
        // drop runs FreeLibrary.
        self.engine = None;
        self.firmwares.clear();
        // Delete the temp copies (and any .dSYM sibling), now that the libraries are
        // unloaded (Windows refuses while a handle is open). Best-effort: a failure
        // warns, never panics.
        for p in self.temp_paths.drain(..) {
            if let Err(e) = std::fs::remove_file(&p) {
                if p.exists() {
                    eprintln!(
                        "         sil: could not delete temp firmware {}: {e}",
                        p.display()
                    );
                }
            }
            let dsym = dsym_bundle(&p);
            if dsym.is_dir() {
                let _ = std::fs::remove_dir_all(&dsym);
            }
        }
        // Release the process lock last.
        self.guard = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn temp_copy_renames_the_dsym_inner_dwarf() {
        let dir = std::env::temp_dir().join(format!("pcs_dsym_test_{}", std::process::id()));
        let lib = dir.join("libfake_fw.dylib");
        let inner = dsym_bundle(&lib)
            .join("Contents")
            .join("Resources")
            .join("DWARF");
        std::fs::create_dir_all(&inner).unwrap();
        std::fs::write(&lib, b"image").unwrap();
        std::fs::write(inner.join("libfake_fw.dylib"), b"dwarf").unwrap();

        let copy = unique_temp_copy(&lib).unwrap();
        let copied_dwarf = dsym_bundle(&copy)
            .join("Contents")
            .join("Resources")
            .join("DWARF")
            .join(copy.file_name().unwrap());
        assert!(
            copied_dwarf.is_file(),
            "the copy's bundle holds the DWARF under the copy's filename: {}",
            copied_dwarf.display()
        );
        assert_eq!(std::fs::read(&copied_dwarf).unwrap(), b"dwarf");

        let _ = std::fs::remove_file(&copy);
        let _ = std::fs::remove_dir_all(dsym_bundle(&copy));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
