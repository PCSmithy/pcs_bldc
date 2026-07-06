//! The native firmware backend: load the firmware shared library, drive it over
//! the control ABI, and read/write its state in-process. It is also the **cvar
//! sample-resolver** — it reads a firmware `static` (any width) and coerces it
//! into the logical [`Value`], and writes a [`Value`] back into firmware memory.
//!
//! This is the in-process boundary from `docs/sil/ffi-boundary.md`, and the
//! only firmware-coupled (unsafe / DWARF) part of the framework; the State Table
//! itself is pure data, fed by this resolver.

use crate::dwarf::{DwarfMap, Leaf, Scalar};
use crate::log::LogLevel;
use crate::member::Member;
use crate::signal::{SignalId, Value};
use crate::state_table::StateTable;
use libloading::{Library, Symbol};
use object::Object;
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

impl Firmware {
    /// Load the firmware shared library and its DWARF, **auto-deriving** the ASLR
    /// anchor (see [`load_with_anchor`](Self::load_with_anchor)).
    pub fn load(path: &Path) -> Result<Self, Box<dyn Error>> {
        Self::load_with_anchor(path, None)
    }

    /// Load the firmware, optionally pinning the ASLR **anchor** symbol.
    ///
    /// The slide (`runtime_addr - link_addr`) is computed from one exported data
    /// global: only the symbol's address is used, never its value, so *any*
    /// exported global that also appears in DWARF works — only the delta matters.
    ///
    /// When `anchor` is `None` the anchor is auto-derived: the first symbol that is
    /// both in the DLL's export table (so `libloading` can resolve its runtime
    /// address) and in the DWARF variable map (so it has a link address) is used.
    /// Pass `Some(name)` only for exotic images where auto-derivation picks a bad
    /// symbol.
    pub fn load_with_anchor(path: &Path, anchor: Option<&str>) -> Result<Self, Box<dyn Error>> {
        // Read the image bytes for the export-table anchor derivation below; load
        // the DWARF via from_lib_path so macOS (DWARF in a sibling .dSYM, not the
        // dylib) works — on ELF/PE it parses the same embedded DWARF as before.
        let bytes = std::fs::read(path)?;
        let dwarf = DwarfMap::from_lib_path(path)?;

        // SAFETY: loading a trusted, project-built artifact.
        let lib = unsafe { Library::new(path)? };

        let anchor = match anchor {
            Some(a) => a.to_string(),
            None => derive_anchor(&bytes, &dwarf)?,
        };

        let link_anchor = dwarf
            .var_addr(&anchor)
            .ok_or("anchor symbol missing from DWARF")?;
        let runtime_anchor = {
            let mut sym_name = anchor.clone().into_bytes();
            sym_name.push(0);
            // SAFETY: `anchor` names an exported data global; we only take the
            // symbol's runtime address (never dereference it), so its type is
            // immaterial — `*mut u8` is just a placeholder pointer type.
            let sym: Symbol<*mut u8> = unsafe { lib.get(&sym_name)? };
            *sym as u64
        };
        let slide = runtime_anchor.wrapping_sub(link_anchor);

        Ok(Self { lib, dwarf, slide })
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

/// Auto-derive an ASLR anchor: the first symbol present in **both** the DLL's
/// export table (so `libloading` can resolve its runtime address) and the DWARF
/// variable map (so it has a link address). Any such global works — only the
/// address delta matters, never the value.
fn derive_anchor(bytes: &[u8], dwarf: &DwarfMap) -> Result<String, Box<dyn Error>> {
    let object = object::File::parse(bytes)?;
    for export in object.exports()? {
        if let Ok(name) = std::str::from_utf8(export.name()) {
            if dwarf.var_addr(name).is_some() {
                return Ok(name.to_string());
            }
        }
    }
    Err("no usable ASLR anchor: no exported global appears in DWARF".into())
}

/// A firmware instance as a [`Member`]: the "firmware kind" of member. It wraps a
/// [`Backend`] (the internal driver of the DLL / lifecycle / DWARF) and drives it
/// on the sim clock, and it is the **only** thing that touches firmware memory —
/// routes never do (they are table-mediated; see [`RouteTable`](crate::route::RouteTable)).
///
/// Constructed with an explicit instance `name` — **not** derived from the DLL, so
/// two boards can run the same firmware image as distinct members — and a firmware
/// tick period. Each [`advance`](Member::advance) accumulates sim time and, per
/// full firmware-tick period elapsed, syncs its two mirror lists around the tick:
///
/// 1. **Flush driven cvars** — read each *driven* cvar's current State Table value
///    and [`write_cvar`](Backend::write_cvar) it into firmware memory. The table
///    value is the **commanded** value (whatever a route or a test last recorded
///    there); firmware memory is what the firmware actually saw.
/// 2. **`advance_tick`** — run the firmware to quiescence.
/// 3. **Sample sampled cvars** — [`read_cvar`](Backend::read_cvar) each *sampled*
///    cvar out of firmware memory and [`record`](StateTable::record) it into the
///    historian.
///
/// Both lists are registered when the member is enabled. Lifecycle
/// (`start` / `shutdown`) stays on the wrapped [`Backend`] handle the driver holds
/// and calls explicitly; [`set_enabled`](Member::set_enabled) registers both the
/// driven and sampled `cvar`s (and, FUTURE, would reload the DLL on re-enable, i.e.
/// boot-from-reset, which is not implemented here).
///
/// [`RouteTable`]: crate::route::RouteTable
pub struct FirmwareMember<'b> {
    name: String,
    backend: &'b dyn Backend,
    tick_period_us: u64,
    /// Sim time accumulated toward the next firmware tick.
    accum_us: u64,
    /// cvars flushed *into* firmware memory before each firmware tick (the
    /// commanded value): `(id, optional unit)`. DWARF path is the id's `name`.
    driven: Vec<(SignalId, Option<String>)>,
    /// cvars sampled *out of* firmware memory into the historian each firmware
    /// tick: `(id, optional unit)`. The DWARF path is the id's `name` segment.
    sampled: Vec<(SignalId, Option<String>)>,
}

impl<'b> FirmwareMember<'b> {
    /// Wrap `backend` as a member named `name`, advancing one firmware tick per
    /// `tick_period_us` of sim time.
    pub fn new(name: &str, backend: &'b dyn Backend, tick_period_us: u64) -> Self {
        Self {
            name: name.to_string(),
            backend,
            tick_period_us,
            accum_us: 0,
            driven: Vec::new(),
            sampled: Vec::new(),
        }
    }

    /// Declare a firmware `cvar` (full [`SignalId`]; DWARF path in its `name`
    /// segment) to sample into the State Table each firmware tick. The signal is
    /// registered when the member is enabled (the engine does this on
    /// [`add_member`](crate::engine::Engine::add_member)); call this before adding
    /// the member.
    pub fn sample_cvar(&mut self, id: SignalId, unit: Option<&str>) {
        self.sampled.push((id, unit.map(str::to_string)));
    }

    /// Declare a firmware `cvar` to **drive**: its State Table value is flushed into
    /// firmware memory (via [`write_cvar`](Backend::write_cvar), by the id's `name`
    /// DWARF path) before each firmware tick. This is the mirror image of
    /// [`sample_cvar`](Self::sample_cvar) — the write side of the `cvar` backing,
    /// and the production path a route takes to reach firmware memory: a source is
    /// routed into this cvar's table entry, and this member flushes it in. The
    /// table value is the *commanded* value; firmware memory is what the firmware
    /// actually saw. Registered when the member is enabled; call before adding it.
    pub fn drive_cvar(&mut self, id: SignalId, unit: Option<&str>) {
        self.driven.push((id, unit.map(str::to_string)));
    }
}

impl Member for FirmwareMember<'_> {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, dt_us: u64, st: &mut StateTable) {
        // Run a firmware tick for each full period elapsed (usually exactly one
        // when the engine tick == the firmware tick). Around each tick: flush the
        // driven cvars into firmware memory, tick, then sample the sampled ones out.
        self.accum_us += dt_us;
        while self.accum_us >= self.tick_period_us {
            self.accum_us -= self.tick_period_us;
            // Flush: commanded table value -> firmware memory. A never-recorded or
            // unregistered driven cvar simply has nothing to flush this tick.
            for (id, _unit) in &self.driven {
                if let Ok(Some(v)) = st.current_value(id) {
                    self.backend.write_cvar(id.name(), v);
                }
            }
            self.backend.advance_tick();
            // Sample: firmware memory -> historian. Registered in set_enabled(true)
            // at add-time; on error log a Warning rather than swallow it.
            for (id, _unit) in &self.sampled {
                let v = self.backend.read_cvar(id.name());
                if let Err(e) = st.record(id, v) {
                    st.log(
                        LogLevel::Warning,
                        &self.name,
                        format!("cvar sample record {id} failed: {e}"),
                    );
                }
            }
        }
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            // Register both mirror lists. Idempotent: a re-enable re-registers
            // identically as a no-op.
            for (id, unit) in self.driven.iter().chain(self.sampled.iter()) {
                let _ = st.register(id.clone(), unit.as_deref());
            }
        }
        // FUTURE: reload the DLL (boot-from-reset) on re-enable; today enable only
        // gates advance (the engine skips a disabled member's tick).
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
    fn firmware_member_advances_firmware_and_samples_cvars() {
        let be = MockBackend::default();
        be.write_cvar("counter", &Value::U32(7));

        let id = SignalId::new("cvar", "dut", "counter", None).unwrap();
        let mut fm = FirmwareMember::new("dut", &be, 1_000);
        fm.sample_cvar(id.clone(), Some("counts"));

        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st); // registers the cvar signal
        assert_eq!(st.len(), 1);

        // One full period -> one firmware tick + one sample.
        st.set_time(1_000);
        fm.advance(1_000, &mut st);
        assert_eq!(*be.ticks.borrow(), 1);
        assert_eq!(st.current_value(&id).unwrap(), Some(&Value::U32(7)));

        // A sub-period advance accumulates but does not tick the firmware.
        fm.advance(500, &mut st);
        assert_eq!(*be.ticks.borrow(), 1);
        // The next 500us completes the period -> a second tick.
        fm.advance(500, &mut st);
        assert_eq!(*be.ticks.borrow(), 2);
    }

    #[test]
    fn firmware_member_flushes_driven_cvars_before_ticking() {
        // The drive side: the table entry's value is flushed into firmware memory
        // before advance_tick each firmware tick. A route/test records the entry;
        // this member pushes it in.
        let be = MockBackend::default();
        let id = SignalId::new("cvar", "dut", "sensor_in", None).unwrap();
        let mut fm = FirmwareMember::new("dut", &be, 1_000);
        fm.drive_cvar(id.clone(), Some("counts"));

        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st); // registers the driven cvar signal
        assert_eq!(st.len(), 1);

        // Command a value into the table entry (as a route would), then advance.
        st.set_time(1_000);
        st.record(&id, Value::U32(42)).unwrap();
        fm.advance(1_000, &mut st);
        assert_eq!(*be.ticks.borrow(), 1);
        // Firmware memory now mirrors the commanded table value.
        assert_eq!(be.read_cvar("sensor_in"), Value::U32(42));

        // Re-command; the next tick flushes the new value.
        st.set_time(2_000);
        st.record(&id, Value::U32(7)).unwrap();
        fm.advance(1_000, &mut st);
        assert_eq!(be.read_cvar("sensor_in"), Value::U32(7));
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
