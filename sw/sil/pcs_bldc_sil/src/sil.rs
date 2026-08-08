//! The [`Sil`] harness: **the simulation itself**. It owns a fresh [`Engine`] and
//! derefs to it, so the whole engine API (`add_member`, `link_duplex`, `step`, …) is
//! called directly on a `Sil`. [`Sil::new`] is a zero-firmware world; firmware is
//! loaded per instance with [`load_firmware`](Sil::load_firmware), each call booting
//! one image copied to its own temp path. Drop dumps a trace, shuts firmwares down,
//! unloads them, and deletes the copies.

use crate::{dll_path, trace, TICK_US};
use std::ops::{Deref, DerefMut};
use std::path::{Path, PathBuf};
use std::rc::{Rc, Weak};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, MutexGuard};
use voyant::{Engine, Firmware, FirmwareMember};

/// Process-global lock, taken at a world's first [`load_firmware`](Sil::load_firmware)
/// and held until drop, so vanilla (threaded) `cargo test` serializes access to the
/// loadable firmware image. Poison is recovered
/// ([`into_inner`](std::sync::PoisonError::into_inner)) so one panicking test releases
/// the lock without cascade-failing the rest. Under cargo-nextest each test runs in
/// its own process, so the lock is uncontended. A model-only world never takes it.
static WORLD_LOCK: Mutex<()> = Mutex::new(());

/// Distinguishes the temp copies within a process (see [`unique_temp_copy`]).
static COPY_COUNTER: AtomicU64 = AtomicU64::new(0);

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
}

impl Sil {
    /// A fresh, zero-firmware world: a new [`Engine`] and nothing else. Takes no lock
    /// and loads no DLL — model-only scenarios are first-class. Add firmware with
    /// [`load_firmware`](Self::load_firmware).
    pub fn new() -> Self {
        Sil {
            engine: Some(Engine::new(TICK_US)),
            firmwares: Vec::new(),
            temp_paths: Vec::new(),
            guard: None,
        }
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

    /// [`load_firmware`](Self::load_firmware) from an explicit library path. Takes the
    /// process lock on the first load of this world. Copies the image to a unique temp
    /// file — `LoadLibrary` on one path aliases the module's statics, so the copy is
    /// what gives each instance (and a repeated load of one path) its own memory.
    /// Loads it, asserts `start()`, hands the member the **sole** strong `Rc` (keeping
    /// only a `Weak` here), wires the temp path as the member's reload recipe (so a
    /// disable/re-enable reboots from reset), tracks the copy for teardown, and returns
    /// the bound [`FirmwareMember`]. Panics if the DLL is missing (build it:
    /// `tools/run_sil.sh`), the copy fails, the load fails, or `start()` returns false.
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
        let mut member = FirmwareMember::new(source_name, fw, TICK_US);
        // Reboot-from-reset recipe: re-enabling this member reloads the same temp copy.
        member.set_reload_path(&copy);
        member
    }

    /// The most-recently loaded firmware handle, for tests that verify firmware MEMORY
    /// directly (route / feedback / mirror checks that read a `static`). The member
    /// owns the strong `Rc`; this upgrades the `Weak` to a shared clone (transient — it
    /// does not perturb the member's sole ownership between steps). Panics in a
    /// zero-firmware world, or if the tracked handle is stale (the member reloaded it).
    pub fn fw(&self) -> Rc<Firmware> {
        self.firmwares
            .last()
            .expect("no firmware loaded")
            .upgrade()
            .expect("firmware handle is stale (member reloaded or unloaded its image)")
    }

    /// Step the engine for `ms` milliseconds of sim time (`ms * 1000 / TICK_US` ticks),
    /// panicking on a step error.
    pub fn run_for_ms(&mut self, ms: u64) {
        let ticks = (ms * 1_000) / TICK_US;
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
/// prefix. Returns the copy's path.
fn unique_temp_copy(src: &Path) -> std::io::Result<PathBuf> {
    let stem = src
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "fw".into());
    let n = COPY_COUNTER.fetch_add(1, Ordering::Relaxed);
    let dst = std::env::temp_dir().join(format!("pcs_sil_{}_{}_{}", std::process::id(), n, stem));
    std::fs::copy(src, &dst)?;
    Ok(dst)
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
        // Delete the temp copies, now that the libraries are unloaded (Windows refuses
        // while a handle is open). Best-effort: a failure warns, never panics.
        for p in self.temp_paths.drain(..) {
            if let Err(e) = std::fs::remove_file(&p) {
                if p.exists() {
                    eprintln!(
                        "         sil: could not delete temp firmware {}: {e}",
                        p.display()
                    );
                }
            }
        }
        // Release the process lock last.
        self.guard = None;
    }
}
