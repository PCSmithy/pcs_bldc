//! The [`Sil`] fresh-world harness: one loaded+booted firmware plus its engine,
//! serialized by a process-global lock and torn down (trace + unload) on drop.

use crate::{dll_path, trace, SOURCE, TICK_US};
use std::rc::Rc;
use std::sync::{Mutex, MutexGuard};
use voyant::{Engine, Firmware, FirmwareMember};

/// Process-global lock, held for a world's whole lifetime so vanilla (threaded)
/// `cargo test` serializes access to the single loadable firmware image. Poison is
/// recovered ([`into_inner`](std::sync::PoisonError::into_inner)) so one panicking
/// test releases the lock without cascade-failing the rest. Under cargo-nextest each
/// test runs in its own process, so the lock is uncontended.
static WORLD_LOCK: Mutex<()> = Mutex::new(());

/// A fresh sim world: this board's firmware DLL loaded and booted from reset, plus
/// the [`Engine`] that drives it. Construct one per `#[test]`; dropping it dumps a
/// trace, shuts the firmware down, and unloads the library (the last [`Rc`] drops),
/// so the next [`Sil::new`] boots firmware statics from scratch.
pub struct Sil {
    /// The simulation: register scenario models on it, add the firmware member
    /// after them (producer-before-consumer order), and step it.
    pub sim: Engine,
    fw: Rc<Firmware>,
    /// Held for the world's lifetime (see [`WORLD_LOCK`]). Declared last so it
    /// releases only after `sim` and `fw` have dropped — i.e. after the DLL unloads,
    /// so no second load overlaps this one.
    _guard: MutexGuard<'static, ()>,
}

impl Sil {
    /// Load and boot a fresh firmware, then build its engine. Acquires the world
    /// lock first, so concurrent `cargo test` threads construct worlds one at a
    /// time. Panics if the DLL is missing (build it first: `tools/run_sil.sh`) or
    /// `sil_fw_start` fails.
    pub fn new() -> Self {
        let guard = WORLD_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        let path = dll_path();
        assert!(
            path.exists(),
            "firmware DLL not found at {}; build it first: tools/run_sil.sh",
            path.display()
        );
        let fw = Rc::new(
            Firmware::load(&path)
                .unwrap_or_else(|e| panic!("failed to load firmware {}: {e}", path.display())),
        );
        assert!(fw.start(), "sil_fw_start() returned false");
        Sil {
            sim: Engine::new(TICK_US),
            fw,
            _guard: guard,
        }
    }

    /// The firmware handle, for tests that deliberately verify firmware MEMORY —
    /// route / feedback / mirror checks that read a `static` straight through it.
    pub fn fw(&self) -> &Firmware {
        &self.fw
    }

    /// A [`FirmwareMember`] bound to this world's firmware. Add it to [`sim`](Self::sim)
    /// **after** the scenario's models so the firmware reads their same-tick outputs;
    /// configure it first (e.g. [`register_cvar_in_state_table`]) when a route drives
    /// an over-threshold cvar.
    ///
    /// [`register_cvar_in_state_table`]: voyant::FirmwareMember::register_cvar_in_state_table
    pub fn firmware_member(&self) -> FirmwareMember {
        FirmwareMember::new(SOURCE, Rc::clone(&self.fw), TICK_US)
    }
}

impl Default for Sil {
    fn default() -> Self {
        Self::new()
    }
}

/// Acquire the process-global world lock directly — for reload experiments (the
/// lifecycle spike) that drive raw [`Firmware`] cycles outside a [`Sil`]. Hold it on
/// one thread for the duration; every [`Sil::new`] blocks until it drops, so no
/// second firmware load overlaps.
pub fn lock_world() -> MutexGuard<'static, ()> {
    WORLD_LOCK.lock().unwrap_or_else(|p| p.into_inner())
}

impl Drop for Sil {
    fn drop(&mut self) {
        // Per-test trace (no-op unless PCS_SIL_TRACE_DIR is set), named after the
        // running test's thread — cargo test names each test thread after its fn.
        let name = std::thread::current().name().unwrap_or("trace").to_string();
        trace::maybe_dump(&self.sim, &name);
        self.fw.shutdown();
    }
}
