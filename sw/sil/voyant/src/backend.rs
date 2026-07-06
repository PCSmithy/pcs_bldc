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
use std::cell::RefCell;
use std::error::Error;
use std::ffi::{c_char, c_void, CStr};
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

    // --- ports (the C→Rust runtime registration seam) ----------------------
    //
    // Firmware members expose **ports**: signals their sim HW drivers register
    // with the framework at runtime through the control-ABI hook vtable, in
    // NATIVE units (volts stay volts; the driver owns any conversion to its
    // C-memory representation — the conversion lives where real hardware does
    // it). Port I/O is **cache-mediated** exactly like the driven/sampled cvar
    // lists — the C side never touches the State Table mid-tick: reads come
    // from an input cache the member fills before each firmware tick, and
    // writes land in an output buffer the member drains after. Defaults are
    // no-ops so a backend without ports needs no code.

    /// Port definitions registered so far, from index `from` (a caller-held
    /// cursor) onward. Definitions are **append-only** with sequential
    /// handles, so a consumer applies `port_defs_since(cursor)` and advances
    /// its cursor by the returned length; a fresh consumer (cursor 0) sees
    /// every port ever registered.
    fn port_defs_since(&self, from: usize) -> Vec<PortDef> {
        let _ = from;
        Vec::new()
    }

    /// Fill (or clear, with `None`) one port's **input cache** slot — what the
    /// firmware's next `readSignal` returns. `None` reads as "never driven",
    /// letting the driver fall back to its default behavior.
    fn set_port_input(&self, handle: i32, value: Option<f64>) {
        let _ = (handle, value);
    }

    /// Drain the **output buffer**: every `(handle, value)` the firmware wrote
    /// via `writeSignal` since the last drain, in write order.
    fn drain_port_writes(&self) -> Vec<(i32, f64)> {
        Vec::new()
    }
}

/// One **port**: a signal a firmware's sim HW driver registered at runtime
/// through the control-ABI hook vtable. The C side names only
/// `{sig_type, local, unit}` — never the `<source>` segment, because the same
/// firmware image may run as several member instances and must not know its
/// instance name; the consuming [`FirmwareMember`] prefixes its own name to
/// form the table id `{sig_type}:{member}:{local}`.
#[derive(Debug, Clone, PartialEq)]
pub struct PortDef {
    /// The handle handed back to C (the index into the backend's port state).
    pub handle: i32,
    /// The signal's backing regime (e.g. `vsig`), as the driver declared it.
    pub sig_type: String,
    /// The driver-local signal name (e.g. an ADC input's config name string).
    pub local: String,
    /// Optional unit metadata (e.g. `V`).
    pub unit: Option<String>,
}

/// The port rendezvous the installed hook vtable targets: the `context`
/// pointer of every trampoline points at one of these, owned (boxed, so its
/// address is stable) by the [`Firmware`] instance.
///
/// **Trampoline safety:** the whole sim is single-threaded (D1), and the C
/// side only calls the hooks while firmware code is executing — inside
/// `start`/`advance_tick`, during which no Rust code holds a borrow of this
/// `RefCell` (the [`FirmwareMember`] syncs caches strictly *around* the tick,
/// never across it). The `RefCell` still catches any future violation loudly.
#[derive(Default)]
struct PortState {
    inner: RefCell<PortsInner>,
}

#[derive(Default)]
struct PortsInner {
    /// Every registered port, append-only; a def's `handle` is its index.
    defs: Vec<PortDef>,
    /// Input cache, indexed by handle: what C `readSignal` returns. `None`
    /// (never driven) reads as false so the driver can fall back.
    inputs: Vec<Option<f64>>,
    /// Output buffer: `(handle, value)` pairs C `writeSignal` produced since
    /// the last drain, in write order.
    writes: Vec<(i32, f64)>,
}

impl PortsInner {
    /// Register (idempotently) and hand back the handle. An exact re-register
    /// of an existing `{sig_type, local, unit}` returns the existing handle,
    /// so a driver re-running its init cannot leak duplicate ports.
    fn register(&mut self, sig_type: &str, local: &str, unit: Option<&str>) -> i32 {
        if let Some(d) = self.defs.iter().find(|d| {
            (d.sig_type == sig_type) && (d.local == local) && (d.unit.as_deref() == unit)
        }) {
            return d.handle;
        }
        let handle = self.defs.len() as i32;
        self.defs.push(PortDef {
            handle,
            sig_type: sig_type.to_string(),
            local: local.to_string(),
            unit: unit.map(str::to_string),
        });
        self.inputs.push(None);
        handle
    }

    fn read(&self, handle: i32) -> Option<f64> {
        usize::try_from(handle)
            .ok()
            .and_then(|i| self.inputs.get(i).copied().flatten())
    }

    fn write(&mut self, handle: i32, value: f64) {
        self.writes.push((handle, value));
    }
}

/// The C-side hook vtable (must match `SIL_ports_hooks_S` in
/// `sw/lib/c/shared/hw/sim/ports/SIL_ports.h` field-for-field). Installed via
/// the firmware's exported `sil_fw_setHooks`, which copies the struct.
#[repr(C)]
struct SilFwHooks {
    context: *mut c_void,
    register_signal:
        unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, *const c_char) -> i32,
    read_signal: unsafe extern "C" fn(*mut c_void, i32, *mut f64) -> bool,
    write_signal: unsafe extern "C" fn(*mut c_void, i32, f64),
}

/// C signature of the firmware's hook-installation export.
type SetHooksFn = unsafe extern "C" fn(*const SilFwHooks);

/// SAFETY (all three trampolines): `ctx` is the address of the `PortState`
/// boxed inside the owning [`Firmware`], installed at load and cleared before
/// unload ([`Firmware::drop`]), so it is valid whenever firmware code can run.
/// Single-threaded; no Rust borrow of the RefCell is live during C execution.
unsafe extern "C" fn port_register_signal(
    ctx: *mut c_void,
    sig_type: *const c_char,
    local: *const c_char,
    unit: *const c_char,
) -> i32 {
    let cstr = |p: *const c_char| {
        if p.is_null() {
            None
        } else {
            CStr::from_ptr(p).to_str().ok()
        }
    };
    match (&*(ctx as *const PortState), cstr(sig_type), cstr(local)) {
        (state, Some(t), Some(l)) => state.inner.borrow_mut().register(t, l, cstr(unit)),
        _ => -1, // NULL / non-UTF-8 names cannot be registered
    }
}

/// See the SAFETY note on [`port_register_signal`].
unsafe extern "C" fn port_read_signal(ctx: *mut c_void, handle: i32, out: *mut f64) -> bool {
    let state = &*(ctx as *const PortState);
    match (state.inner.borrow().read(handle), out.is_null()) {
        (Some(v), false) => {
            *out = v;
            true
        }
        _ => false,
    }
}

/// See the SAFETY note on [`port_register_signal`].
unsafe extern "C" fn port_write_signal(ctx: *mut c_void, handle: i32, value: f64) {
    let state = &*(ctx as *const PortState);
    state.inner.borrow_mut().write(handle, value);
}

/// A loaded firmware instance (one per process — see ffi-boundary.md §1).
pub struct Firmware {
    lib: Library,
    dwarf: DwarfMap,
    /// runtime_addr - link_addr, applied to every DWARF address (ASLR slide).
    slide: u64,
    /// Port rendezvous the hook vtable's `context` points at. Boxed so its
    /// address survives moves of the `Firmware` value itself.
    ports: Box<PortState>,
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

        // Install the port hook vtable BEFORE any `start`, so drivers can
        // register ports during init. Optional export: a firmware without the
        // seam still loads (it just has no ports). The C side copies the
        // struct, so the stack-local vtable need not outlive this call; the
        // boxed PortState the context points at lives as long as `self`.
        let ports = Box::new(PortState::default());
        unsafe {
            if let Ok(set_hooks) = lib.get::<SetHooksFn>(b"sil_fw_setHooks\0") {
                let hooks = SilFwHooks {
                    context: std::ptr::from_ref::<PortState>(&ports).cast_mut().cast::<c_void>(),
                    register_signal: port_register_signal,
                    read_signal: port_read_signal,
                    write_signal: port_write_signal,
                };
                set_hooks(&hooks);
            }
        }

        Ok(Self {
            lib,
            dwarf,
            slide,
            ports,
        })
    }

    fn resolve(&self, path: &str) -> (*mut u8, Leaf) {
        let (link, leaf) = self
            .dwarf
            .resolve(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        (link.wrapping_add(self.slide) as *mut u8, leaf)
    }
}

impl Drop for Firmware {
    fn drop(&mut self) {
        // Uninstall the hooks before the DLL unloads so no window exists in
        // which C could call into a dangling context (belt-and-braces: the sim
        // is single-threaded, so nothing can run concurrently anyway).
        unsafe {
            if let Ok(set_hooks) = self.lib.get::<SetHooksFn>(b"sil_fw_setHooks\0") {
                set_hooks(std::ptr::null());
            }
        }
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

    fn port_defs_since(&self, from: usize) -> Vec<PortDef> {
        let inner = self.ports.inner.borrow();
        inner.defs.get(from..).map(<[PortDef]>::to_vec).unwrap_or_default()
    }

    fn set_port_input(&self, handle: i32, value: Option<f64>) {
        let mut inner = self.ports.inner.borrow_mut();
        if let Some(slot) = usize::try_from(handle)
            .ok()
            .and_then(|i| inner.inputs.get_mut(i))
        {
            *slot = value;
        }
    }

    fn drain_port_writes(&self) -> Vec<(i32, f64)> {
        std::mem::take(&mut self.ports.inner.borrow_mut().writes)
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
/// full firmware-tick period elapsed, syncs its mirror lists around the tick:
///
/// 1. **Apply pending port registrations** — turn each new [`PortDef`] the
///    backend accumulated (a sim HW driver registered it through the hook
///    vtable) into a table entry `{sig_type}:{member_name}:{local}`
///    (idempotent; this member prefixes its own instance name).
/// 2. **Flush port input caches** — for **every** registered port, cache the
///    table entry's current value in the backend ([`set_port_input`]); a port
///    never driven caches `None`, which the firmware's `readSignal` reports as
///    false (the driver falls back). **Input ports carry commanded values**
///    (table → cache → C read).
/// 3. **Flush driven cvars** — read each *driven* cvar's current State Table value
///    and [`write_cvar`](Backend::write_cvar) it into firmware memory. The table
///    value is the **commanded** value (whatever a route or a test last recorded
///    there); firmware memory is what the firmware actually saw.
/// 4. **`advance_tick`** — run the firmware to quiescence.
/// 5. **Drain port writes** — record every value the firmware wrote via
///    `writeSignal` into its port's table entry ([`drain_port_writes`]).
///    **Output ports carry firmware-produced values** (C write → table). The
///    same port may do both — the table is the rendezvous; no direction
///    metadata exists on a signal.
/// 6. **Sample sampled cvars** — [`read_cvar`](Backend::read_cvar) each *sampled*
///    cvar out of firmware memory and [`record`](StateTable::record) it into the
///    historian.
///
/// Port I/O is cache-mediated end to end — deterministic, with **no mid-tick
/// State Table access from C** (mirroring how driven/sampled cvars sync their
/// firmware-memory mirrors).
///
/// The cvar lists are registered when the member is enabled; **pending port
/// registrations are also applied there**, so ports registered during
/// `sil_fw_start` become table entries as soon as the member is added to an
/// engine (else at its next advance). Lifecycle (`start` / `shutdown`) stays on
/// the wrapped [`Backend`] handle the driver holds and calls explicitly
/// (FUTURE: re-enable would reload the DLL, i.e. boot-from-reset — not
/// implemented here).
///
/// [`RouteTable`]: crate::route::RouteTable
/// [`set_port_input`]: Backend::set_port_input
/// [`drain_port_writes`]: Backend::drain_port_writes
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
    /// Ports this member has applied to the table: `(handle, id)`, in
    /// registration order (deterministic iteration).
    ports: Vec<(i32, SignalId)>,
    /// How many of the backend's port defs this member has consumed.
    port_cursor: usize,
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
            ports: Vec::new(),
            port_cursor: 0,
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

    /// Apply the backend's pending port registrations to the table: each new
    /// [`PortDef`] becomes an entry `{sig_type}:{member_name}:{local}` (this
    /// member prefixes its own instance name — the C side never knows it).
    /// Registration is idempotent on the table, so re-applying across a
    /// reboot/re-enable preserves the entry's history. A def with an invalid
    /// id or a conflicting unit is logged as a Warning and skipped.
    fn apply_pending_ports(&mut self, st: &mut StateTable) {
        let defs = self.backend.port_defs_since(self.port_cursor);
        self.port_cursor += defs.len();
        for def in defs {
            match SignalId::new(&def.sig_type, &self.name, &def.local, None) {
                Ok(id) => match st.register(id.clone(), def.unit.as_deref()) {
                    Ok(()) => self.ports.push((def.handle, id)),
                    Err(e) => st.log(
                        LogLevel::Warning,
                        &self.name,
                        format!("port {id} register failed: {e}"),
                    ),
                },
                Err(e) => st.log(
                    LogLevel::Warning,
                    &self.name,
                    format!("port {:?} yields an invalid signal id: {e}", def.local),
                ),
            }
        }
    }
}

impl Member for FirmwareMember<'_> {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, dt_us: u64, st: &mut StateTable) {
        // Run a firmware tick for each full period elapsed (usually exactly one
        // when the engine tick == the firmware tick). Around each tick, in this
        // order: apply pending port registrations; flush the port input caches
        // and the driven cvars in; tick; drain the port writes and sample the
        // sampled cvars out. All mirror-synced — C never touches the table.
        self.accum_us += dt_us;
        while self.accum_us >= self.tick_period_us {
            self.accum_us -= self.tick_period_us;
            self.apply_pending_ports(st);
            // Input ports: cache each port's commanded table value for the C
            // side to read. Every port gets a cache slot every tick; a port
            // never driven (or holding a non-numeric Value) caches None, which
            // readSignal reports as false -> the driver falls back.
            for (handle, id) in &self.ports {
                let v = st.current_value(id).ok().flatten().and_then(value_to_f64);
                self.backend.set_port_input(*handle, v);
            }
            // Flush: commanded table value -> firmware memory. A never-recorded or
            // unregistered driven cvar simply has nothing to flush this tick.
            for (id, _unit) in &self.driven {
                if let Ok(Some(v)) = st.current_value(id) {
                    self.backend.write_cvar(id.name(), v);
                }
            }
            self.backend.advance_tick();
            // Output ports: drain what the firmware wrote into the historian.
            for (handle, value) in self.backend.drain_port_writes() {
                match self.ports.iter().find(|(h, _)| *h == handle) {
                    Some((_, id)) => {
                        if let Err(e) = st.record(id, Value::F64(value)) {
                            st.log(
                                LogLevel::Warning,
                                &self.name,
                                format!("port write record {id} failed: {e}"),
                            );
                        }
                    }
                    None => st.log(
                        LogLevel::Warning,
                        &self.name,
                        format!("port write to unapplied handle {handle} dropped"),
                    ),
                }
            }
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
            // Ports registered before this member existed (typically during
            // `sil_fw_start`, which the driver calls before adding the member)
            // become table entries here — i.e. immediately at add_member — so
            // routes into them are valid from the first engine step. Later
            // registrations apply at the member's next advance.
            self.apply_pending_ports(st);
        }
        // FUTURE: reload the DLL (boot-from-reset) on re-enable; today enable only
        // gates advance (the engine skips a disabled member's tick).
    }
}

/// Coerce a table [`Value`] into the port seam's `f64` currency (`double`
/// scalars only for now; typed variants are the documented extension path).
/// Numeric variants coerce; `Enum`/`Bytes` cannot drive a port and read as
/// "not driven".
fn value_to_f64(v: &Value) -> Option<f64> {
    match v {
        Value::F64(x) => Some(*x),
        Value::F32(x) => Some(f64::from(*x)),
        Value::I32(x) => Some(f64::from(*x)),
        Value::U32(x) => Some(f64::from(*x)),
        Value::U64(x) => Some(*x as f64),
        Value::Bool(x) => Some(f64::from(u8::from(*x))),
        Value::Enum(_) | Value::Bytes(_) => None,
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

    // --- ports ------------------------------------------------------------

    use std::ffi::CString;

    /// A port-capable mock backend: its "firmware" reads port handle 0 each
    /// tick and, when driven, writes twice that value to port handle 1 —
    /// enough to prove the full cache-mediated loop through a FirmwareMember.
    #[derive(Default)]
    struct PortMock {
        ports: PortState,
        /// What the mock firmware saw on port 0 at its last tick.
        seen: RefCell<Option<f64>>,
    }

    impl Backend for PortMock {
        fn start(&self) -> bool {
            true
        }
        fn advance_tick(&self) {
            let seen = self.ports.inner.borrow().read(0);
            *self.seen.borrow_mut() = seen;
            if let Some(v) = seen {
                self.ports.inner.borrow_mut().write(1, v * 2.0);
            }
        }
        fn shutdown(&self) {}
        fn read_cvar(&self, _path: &str) -> Value {
            Value::U32(0)
        }
        fn write_cvar(&self, _path: &str, _v: &Value) {}

        fn port_defs_since(&self, from: usize) -> Vec<PortDef> {
            let inner = self.ports.inner.borrow();
            inner.defs.get(from..).map(<[PortDef]>::to_vec).unwrap_or_default()
        }
        fn set_port_input(&self, handle: i32, value: Option<f64>) {
            let mut inner = self.ports.inner.borrow_mut();
            if let Some(slot) = usize::try_from(handle)
                .ok()
                .and_then(|i| inner.inputs.get_mut(i))
            {
                *slot = value;
            }
        }
        fn drain_port_writes(&self) -> Vec<(i32, f64)> {
            std::mem::take(&mut self.ports.inner.borrow_mut().writes)
        }
    }

    fn port_mock_with_in_out() -> PortMock {
        let mock = PortMock::default();
        {
            let mut inner = mock.ports.inner.borrow_mut();
            assert_eq!(inner.register("vsig", "in_v", Some("V")), 0);
            assert_eq!(inner.register("vsig", "out_v", Some("V")), 1);
        }
        mock
    }

    #[test]
    fn ports_inner_register_is_idempotent_with_sequential_handles() {
        let mut inner = PortsInner::default();
        assert_eq!(inner.register("vsig", "a", Some("V")), 0);
        assert_eq!(inner.register("vsig", "b", None), 1);
        // Exact re-register returns the existing handle; no duplicate def.
        assert_eq!(inner.register("vsig", "a", Some("V")), 0);
        assert_eq!(inner.defs.len(), 2);
        // A different unit is a different port (the table will flag the
        // conflict when the member applies it).
        assert_eq!(inner.register("vsig", "a", Some("mV")), 2);
    }

    #[test]
    fn firmware_member_applies_ports_and_mediates_io() {
        // The full mirror-synced port loop: table -> input cache -> C read,
        // then C write -> output buffer -> table. Native format end to end.
        let mock = port_mock_with_in_out();
        let mut fm = FirmwareMember::new("dut", &mock, 1_000);
        let mut st = StateTable::new();

        // set_enabled applies the pending registrations immediately.
        fm.set_enabled(true, &mut st);
        let in_id = SignalId::parse("vsig:dut:in_v").unwrap();
        let out_id = SignalId::parse("vsig:dut:out_v").unwrap();
        assert_eq!(st.len(), 2);
        assert_eq!(st.current_value(&in_id).unwrap(), None);

        // Command 1.5 V into the input port (as a route would); one tick later
        // the mock firmware saw exactly 1.5 and its write landed in the table.
        st.set_time(1_000);
        st.record(&in_id, Value::F64(1.5)).unwrap();
        fm.advance(1_000, &mut st);
        assert_eq!(*mock.seen.borrow(), Some(1.5));
        assert_eq!(st.current_value(&out_id).unwrap(), Some(&Value::F64(3.0)));
    }

    #[test]
    fn undriven_port_reads_as_not_driven() {
        let mock = port_mock_with_in_out();
        let mut fm = FirmwareMember::new("dut", &mock, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);

        // Never-driven input: the firmware's read comes back "not driven"
        // (None), and no port write is produced.
        st.set_time(1_000);
        fm.advance(1_000, &mut st);
        assert_eq!(*mock.seen.borrow(), None);
        let out_id = SignalId::parse("vsig:dut:out_v").unwrap();
        assert_eq!(st.current_value(&out_id).unwrap(), None);

        // Non-numeric commanded value also reads as not driven.
        let in_id = SignalId::parse("vsig:dut:in_v").unwrap();
        st.set_time(2_000);
        st.record(&in_id, Value::Enum("ON".into())).unwrap();
        fm.advance(1_000, &mut st);
        assert_eq!(*mock.seen.borrow(), None);
    }

    #[test]
    fn port_registered_mid_run_applies_at_next_advance() {
        let mock = port_mock_with_in_out();
        let mut fm = FirmwareMember::new("dut", &mock, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);
        assert_eq!(st.len(), 2);

        // A driver registers a third port later (open registration, any time);
        // it becomes a table entry at the member's next advance.
        mock.ports.inner.borrow_mut().register("vsig", "late_v", None);
        st.set_time(1_000);
        fm.advance(1_000, &mut st);
        assert_eq!(st.len(), 3);
        let late = SignalId::parse("vsig:dut:late_v").unwrap();
        assert_eq!(st.current_value(&late).unwrap(), None);
    }

    #[test]
    fn trampolines_roundtrip_the_c_abi() {
        // Drive the extern "C" trampolines exactly as the C helper does.
        let state = PortState::default();
        let ctx = std::ptr::from_ref(&state).cast_mut().cast::<c_void>();
        let sig_type = CString::new("vsig").unwrap();
        let local = CString::new("adc_in").unwrap();
        let unit = CString::new("V").unwrap();

        let h = unsafe {
            port_register_signal(ctx, sig_type.as_ptr(), local.as_ptr(), unit.as_ptr())
        };
        assert_eq!(h, 0);
        assert_eq!(
            state.inner.borrow().defs[0],
            PortDef {
                handle: 0,
                sig_type: "vsig".into(),
                local: "adc_in".into(),
                unit: Some("V".into()),
            }
        );
        // NULL unit -> None; NULL name -> registration refused.
        let h2 = unsafe {
            port_register_signal(ctx, sig_type.as_ptr(), local.as_ptr(), std::ptr::null())
        };
        assert_eq!(h2, 1);
        assert_eq!(state.inner.borrow().defs[1].unit, None);
        let bad =
            unsafe { port_register_signal(ctx, std::ptr::null(), local.as_ptr(), std::ptr::null()) };
        assert_eq!(bad, -1);

        // Read: false while undriven, true (with the value) once cached.
        let mut out = 0.0f64;
        assert!(!unsafe { port_read_signal(ctx, h, &mut out) });
        state.inner.borrow_mut().inputs[0] = Some(2.5);
        assert!(unsafe { port_read_signal(ctx, h, &mut out) });
        assert_eq!(out, 2.5);
        // Bogus handle / NULL out are refused.
        assert!(!unsafe { port_read_signal(ctx, 99, &mut out) });
        assert!(!unsafe { port_read_signal(ctx, h, std::ptr::null_mut()) });

        // Write: appended to the output buffer in order.
        unsafe { port_write_signal(ctx, h, 7.25) };
        unsafe { port_write_signal(ctx, h2, -1.0) };
        assert_eq!(state.inner.borrow().writes, vec![(0, 7.25), (1, -1.0)]);
    }
}
