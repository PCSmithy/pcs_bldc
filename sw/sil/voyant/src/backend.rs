//! The firmware member and its execution seam. [`Firmware`] is the public handle:
//! it loads the shared library, drives it over the control ABI
//! (`start`/`advance_tick`/`shutdown`), and is the **cvar sample-resolver** —
//! reading/writing a firmware `static` (any width) as a logical [`Value`].
//! [`FirmwareMember`] wraps it as a [`Member`], the public seam the engine drives.
//!
//! The `Backend` trait is **internal plumbing**: the execution / test-double seam
//! [`FirmwareMember`] drives around each tick (so mock backends prove member/engine
//! semantics without a real DLL) — not a public seam ([`Member`] is).
//!
//! This is the in-process boundary from `docs/sil/ffi-boundary.md`, the only
//! firmware-coupled (unsafe / DWARF) part of the framework; the State Table is pure
//! data fed by this resolver.

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

/// The **internal execution seam** [`FirmwareMember`] drives one firmware instance
/// through around each tick: `advance_tick`, white-box `cvar` read/write by path, and
/// the port registration seam. Crate plumbing, **not** a public seam ([`Member`] is);
/// it exists so in-crate tests can stand up **mock backends** without a real DLL.
///
/// Lifecycle (`start`/`shutdown`) and construction ([`Firmware::load`]) stay **off**
/// the trait — called explicitly on the concrete [`Firmware`] handle the driver holds.
///
/// All methods take `&self`: a backend mutates *external* state (firmware memory /
/// execution), not the Rust handle.
pub(crate) trait Backend {
    /// Advance one sim tick (run the firmware to its next quiescence).
    fn advance_tick(&self);

    /// Sample a firmware `static` by path into a logical [`Value`] — the read
    /// side of the State Table's `cvar` backing.
    fn read_cvar(&self, path: &str) -> Value;

    /// Write a logical [`Value`] into a firmware `static` by path — white-box
    /// injection (the write side of the `cvar` backing).
    fn write_cvar(&self, path: &str, v: &Value);

    /// Enumerate this firmware's traceable `cvar` leaves (every scalar/enum leaf under
    /// every `static`, nested members + array elements expanded), with the exclusion
    /// policy (array-size `threshold`, `includes`) applied. Each leaf carries an
    /// optional pre-resolved [`CvarHandle`] so the per-tick sweep never re-resolves
    /// DWARF. Default: empty (a backend with no DWARF has no leaves).
    fn enumerate_cvars(&self, threshold: usize, includes: &[String]) -> CvarEnumeration {
        let _ = (threshold, includes);
        CvarEnumeration::default()
    }

    /// Fast-read a leaf via its [`CvarHandle`] — the sweep's hot path, bypassing path
    /// parsing / DWARF lookup. Only called with a handle this backend produced; the
    /// default is unreachable.
    fn read_cvar_resolved(&self, handle: CvarHandle) -> Value {
        let _ = handle;
        unreachable!("backend produced no cvar handles but was asked to read one")
    }

    /// Fast-read a leaf as a **native scalar** ([`ScalarSample`]) — the typed-decode
    /// path feeding the State Table's typed fast lanes, so a changing scalar leaf never
    /// constructs a `Value`. The default wraps [`read_cvar_resolved`](Self::read_cvar_resolved)
    /// as [`ScalarSample::Boxed`] (a `Value`-only backend still works via the generic
    /// path); a real firmware overrides to decode natively.
    fn read_cvar_scalar(&self, handle: CvarHandle) -> ScalarSample {
        ScalarSample::Boxed(self.read_cvar_resolved(handle))
    }

    /// The **memory layout** of a resolved leaf (runtime address + byte size), used by
    /// the Tier-1 shadow sweep to group leaves into contiguous ranges. Only called with
    /// a handle this backend produced; the default is unreachable (a handle-less backend
    /// falls back to the string-path sweep).
    fn cvar_layout(&self, handle: CvarHandle) -> (u64, usize) {
        let _ = handle;
        unreachable!("backend produced no cvar handles but was asked for a layout")
    }

    /// Whether `shadow.len()` bytes of **live** firmware memory at `addr` equal
    /// `shadow` — the shadow sweep's per-range fast path (a bare `memcmp` avoids the
    /// bulk read for unchanged ranges). Same address discipline as
    /// [`read_range`](Self::read_range).
    fn range_eq(&self, addr: u64, shadow: &[u8]) -> bool {
        let _ = (addr, shadow);
        unreachable!("backend produced no cvar layouts but was asked to compare a range")
    }

    /// Copy `buf.len()` bytes of **live** firmware memory at `addr` into `buf` — the
    /// shadow sweep's bulk read (only for a changed range). `addr`/`len` derive from
    /// resolved leaf layouts ([`cvar_layout`](Self::cvar_layout)), so the range lies
    /// within real firmware statics. Default is unreachable.
    fn read_range(&self, addr: u64, buf: &mut [u8]) {
        let _ = (addr, buf);
        unreachable!("backend produced no cvar layouts but was asked to read a range")
    }

    // --- ports (the C→Rust runtime registration seam) ----------------------
    //
    // Firmware members expose **ports**: signals their sim HW drivers register at
    // runtime through the control-ABI hook vtable, in NATIVE units (the driver owns
    // any conversion, where real hardware does it). Port I/O is **cache-mediated** —
    // the C side never touches the State Table mid-tick: reads come from an input
    // cache the member fills before each tick, writes land in an output buffer it
    // drains after. Defaults are no-ops so a portless backend needs no code.

    /// Port definitions registered from index `from` onward. Definitions are
    /// **append-only** with sequential handles, so a consumer applies
    /// `port_defs_since(cursor)` and advances its cursor by the returned length.
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

/// An opaque, backend-private token for a **pre-resolved** `cvar` leaf: whatever
/// the backend needs to read that leaf without re-resolving its path (for
/// [`Firmware`], an index into its address/type resolution cache). Handed out by
/// [`Backend::enumerate_cvars`] and read back via [`Backend::read_cvar_resolved`];
/// never inspected outside the backend, so it stays off the public API.
#[derive(Clone, Copy, Debug)]
pub(crate) struct CvarHandle(usize);

/// A leaf decoded as a **native scalar** for the sweep's typed fast lane
/// ([`Backend::read_cvar_scalar`]): scalar variants carry the value unwrapped;
/// [`ScalarSample::Boxed`] holds anything non-scalar (an enum leaf's symbolic
/// [`Value::Enum`]) or a default backend's `Value`. Variant widths mirror
/// [`scalar_to_value`]'s coercion so a leaf lands in the same typed column either path.
pub(crate) enum ScalarSample {
    F32(f32),
    F64(f64),
    I32(i32),
    U32(u32),
    U64(u64),
    Bool(bool),
    Boxed(Value),
}

/// The result of [`Backend::enumerate_cvars`]: the traceable leaves plus what the
/// exclusion policy dropped (reported once at member enable).
#[derive(Default)]
pub(crate) struct CvarEnumeration {
    /// Each traceable leaf: its DWARF path and an optional fast-read handle
    /// (`None` → the member falls back to the string-path [`Backend::read_cvar`]).
    pub leaves: Vec<(String, Option<CvarHandle>)>,
    /// Arrays skipped whole by the size threshold / multi-dim / unknown-length rule.
    pub excluded_arrays: usize,
    /// Leaves skipped as non-traceable types (pointer/function/opaque/unresolvable).
    pub skipped_leaves: usize,
    /// A recursion/leaf-budget safety cap was hit during enumeration.
    pub capped: bool,
}

/// One **port**: a signal a firmware's sim HW driver registered at runtime
/// through the control-ABI hook vtable. The C side names only
/// `{sig_type, local, unit}` — never the `<source>` segment, because the same
/// firmware image may run as several member instances and must not know its
/// instance name; the consuming [`FirmwareMember`] prefixes its own name to
/// form the table id `{sig_type}:{member}:{local}`.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct PortDef {
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
    /// Fast-read resolution cache for [`CvarHandle`]s: `(runtime addr, leaf type)`
    /// resolved once at leaf enumeration, indexed by the handle. The per-tick
    /// mirror sweep reads straight from here — no DWARF lookup, no path parsing.
    cvar_cache: RefCell<Vec<(*mut u8, Leaf)>>,
}

impl Firmware {
    /// Load the firmware shared library and its DWARF, **auto-deriving** the ASLR
    /// anchor (see [`load_with_anchor`](Self::load_with_anchor)).
    pub fn load(path: &Path) -> Result<Self, Box<dyn Error>> {
        Self::load_with_anchor(path, None)
    }

    /// Load the firmware, optionally pinning the ASLR **anchor** symbol.
    ///
    /// The slide (`runtime_addr - link_addr`) is computed from one exported symbol:
    /// only the symbol's address is used, never its value, so *any* exported symbol
    /// that also appears in DWARF works — only the delta matters. The link address
    /// comes from DWARF (a variable's `DW_OP_addr`, or a function's `DW_AT_low_pc`);
    /// the runtime address from `libloading` — the same basis for both, since a data
    /// global and a function are equally slid by the loader.
    ///
    /// When `anchor` is `None` the anchor is auto-derived (see [`derive_anchor`]):
    /// an exported **variable** in DWARF is preferred, falling back to an exported
    /// **function** in DWARF (the ELF/LTO case, where the exported data statics are
    /// absent from the `.dynsym` while the `sil_fw_*` control-ABI functions are
    /// always exported and always in DWARF). Pass `Some(name)` — a variable *or* a
    /// function name — only for exotic images where auto-derivation picks badly.
    pub fn load_with_anchor(path: &Path, anchor: Option<&str>) -> Result<Self, Box<dyn Error>> {
        // Read the image bytes for the export-table anchor derivation below; load
        // the DWARF via from_lib_path so macOS (DWARF in a sibling .dSYM, not the
        // dylib) works — on ELF/PE it parses the same embedded DWARF as before.
        let bytes = std::fs::read(path)?;
        let dwarf = DwarfMap::from_lib_path(path)?;

        // SAFETY: loading a trusted, project-built artifact.
        let lib = unsafe { Library::new(path)? };

        // Pick the anchor symbol and its kind (which DWARF map holds its link
        // address). An explicit override may name a variable or a function.
        let (anchor, kind) = match anchor {
            Some(a) if dwarf.var_addr(a).is_some() => (a.to_string(), AnchorKind::Var),
            Some(a) if dwarf.func_addr(a).is_some() => (a.to_string(), AnchorKind::Func),
            Some(_) => return Err("anchor symbol missing from DWARF".into()),
            None => derive_anchor(&bytes, &dwarf)?,
        };

        let link_anchor = match kind {
            AnchorKind::Var => dwarf.var_addr(&anchor),
            AnchorKind::Func => dwarf.func_addr(&anchor),
        }
        .ok_or("anchor symbol missing from DWARF")?;
        let runtime_anchor = {
            let mut sym_name = anchor.clone().into_bytes();
            sym_name.push(0);
            // SAFETY: `anchor` names an exported symbol (a data global or a
            // function). We only take the symbol's runtime address (never
            // dereference / call it), so its type is immaterial — `*mut u8` is just
            // a placeholder; `libloading` returns the loader's address for either.
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
            cvar_cache: RefCell::new(Vec::new()),
        })
    }

    fn resolve(&self, path: &str) -> (*mut u8, Leaf) {
        let (link, leaf) = self
            .dwarf
            .resolve(path)
            .unwrap_or_else(|| panic!("DWARF path not found: {path}"));
        (link.wrapping_add(self.slide) as *mut u8, leaf)
    }

    /// Resolve a cvar path to its runtime address (DWARF link address + ASLR
    /// slide), or `None` if the path is not in DWARF. Diagnostic aid: exposes
    /// the resolved address so a caller can detect when two *distinct* statics
    /// collapse to the *same* address — a per-variable DWARF-resolution fault
    /// (observed for file-scope `static`s on Mach-O). Does not read memory.
    pub fn resolve_addr(&self, path: &str) -> Option<usize> {
        self.dwarf
            .resolve(path)
            .map(|(link, _)| link.wrapping_add(self.slide) as usize)
    }

    // --- white-box lifecycle + cvar access (the public firmware handle) -----
    //
    // These are the operations a driver reaches for while holding `&Firmware`
    // for ad-hoc injection/inspection alongside the engine. The internal
    // [`Backend`] impl below forwards to them; the trait exists only for the
    // in-crate mock-backend test seam.

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

    /// Sample a firmware `static` by DWARF path into a logical [`Value`]
    /// (the cvar sample-resolver). Scalar widths are coerced; an enum field
    /// reads as its symbolic [`Value::Enum`] name (or `<n>` for an unknown
    /// enumerator, e.g. a bitwise combination).
    pub fn read_cvar(&self, path: &str) -> Value {
        let (p, leaf) = self.resolve(path);
        self.read_leaf(p, leaf)
    }

    /// Read an already-resolved leaf (runtime address + leaf type) into a
    /// [`Value`] — shared by [`read_cvar`](Self::read_cvar) (resolve-then-read) and
    /// the [`CvarHandle`] fast path (cache-then-read).
    fn read_leaf(&self, p: *mut u8, leaf: Leaf) -> Value {
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

    /// The byte size of a resolved [`Leaf`] — a scalar's fixed width, or an enum's
    /// DWARF `byte_size`. Backs [`cvar_layout`](Backend::cvar_layout) so the shadow
    /// sweep can size each leaf's footprint in memory.
    fn leaf_size(&self, leaf: Leaf) -> usize {
        match leaf {
            Leaf::Scalar(kind) => scalar_byte_size(kind),
            Leaf::Enum(off) => self.dwarf.enum_size(off).unwrap() as usize,
        }
    }

    /// Enumerate traceable `cvar` leaves (via [`DwarfMap::enumerate_leaves`]) and
    /// resolve each to a cached [`CvarHandle`] (runtime addr + leaf type) so the
    /// per-tick mirror sweep never re-resolves DWARF. A leaf the enumerator emitted
    /// but the resolver cannot place is counted as skipped (should not happen —
    /// both walk the same maps — but keeps the contract honest).
    fn enumerate_cvars_impl(&self, threshold: usize, includes: &[String]) -> CvarEnumeration {
        let en = self.dwarf.enumerate_leaves(threshold, includes);
        let mut leaves = Vec::with_capacity(en.paths.len());
        let mut skipped = en.skipped_leaves;
        for path in en.paths {
            match self.dwarf.resolve(&path) {
                Some((link, leaf)) => {
                    let addr = link.wrapping_add(self.slide) as *mut u8;
                    let mut cache = self.cvar_cache.borrow_mut();
                    let handle = CvarHandle(cache.len());
                    cache.push((addr, leaf));
                    leaves.push((path, Some(handle)));
                }
                None => skipped += 1,
            }
        }
        CvarEnumeration {
            leaves,
            excluded_arrays: en.excluded_arrays,
            skipped_leaves: skipped,
            capped: en.capped,
        }
    }

    /// Write a logical [`Value`] into a firmware `static` by DWARF path. Scalars
    /// coerce to the field's width; an enum accepts [`Value::Enum`] (name → its
    /// value) or a raw `U32`/`I32`. Panics on an incompatible variant.
    pub fn write_cvar(&self, path: &str, v: &Value) {
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

/// The internal [`Backend`] impl forwards lifecycle + `cvar` access to the
/// inherent [`Firmware`] methods (the public handle) and adds the port seam.
impl Backend for Firmware {
    fn advance_tick(&self) {
        Firmware::advance_tick(self);
    }

    fn read_cvar(&self, path: &str) -> Value {
        Firmware::read_cvar(self, path)
    }

    fn write_cvar(&self, path: &str, v: &Value) {
        Firmware::write_cvar(self, path, v);
    }

    fn enumerate_cvars(&self, threshold: usize, includes: &[String]) -> CvarEnumeration {
        self.enumerate_cvars_impl(threshold, includes)
    }

    fn read_cvar_resolved(&self, handle: CvarHandle) -> Value {
        let (p, leaf) = self.cvar_cache.borrow()[handle.0];
        self.read_leaf(p, leaf)
    }

    fn read_cvar_scalar(&self, handle: CvarHandle) -> ScalarSample {
        let (p, leaf) = self.cvar_cache.borrow()[handle.0];
        // SAFETY: valid firmware address (DWARF + slide); firmware quiescent. A
        // scalar decodes natively (no `Value`); an enum leaf reads as its symbolic
        // name and rides the boxed lane.
        match leaf {
            Leaf::Scalar(kind) => unsafe { scalar_to_sample(p, kind) },
            Leaf::Enum(_) => ScalarSample::Boxed(self.read_leaf(p, leaf)),
        }
    }

    fn cvar_layout(&self, handle: CvarHandle) -> (u64, usize) {
        let (p, leaf) = self.cvar_cache.borrow()[handle.0];
        (p as u64, self.leaf_size(leaf))
    }

    fn range_eq(&self, addr: u64, shadow: &[u8]) -> bool {
        // SAFETY: see `read_range` — the span lies within loaded firmware statics
        // and the firmware is quiescent. We form a shared `&[u8]` over initialized
        // bytes we never mutate and only compare it.
        let live = unsafe { std::slice::from_raw_parts(addr as *const u8, shadow.len()) };
        live == shadow
    }

    fn read_range(&self, addr: u64, buf: &mut [u8]) {
        // SAFETY: `addr..addr+buf.len()` lies inside the firmware's own
        // `.data`/`.bss`: the range spans from the lowest to the highest resolved
        // leaf address in a contiguity group (leaves are real statics at
        // DWARF-resolved runtime addresses), merged only across small gaps, so
        // every byte — including inter-static padding in a gap — is backed by the
        // loaded image and readable. The firmware is quiescent (single-threaded;
        // no firmware code runs during out-sync), and the read is a plain byte copy
        // (no aliasing of a typed reference).
        unsafe {
            std::ptr::copy_nonoverlapping(addr as *const u8, buf.as_mut_ptr(), buf.len());
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

/// Which DWARF map holds a chosen anchor's link address — a variable's
/// `DW_OP_addr` or a function's `DW_AT_low_pc`. Both share the loader's slide, so
/// this only selects where the link address is read from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AnchorKind {
    Var,
    Func,
}

/// Auto-derive an ASLR anchor: the first exported symbol that also appears in
/// DWARF. **Variables are preferred** (the historical strategy); if none of the
/// exported symbols is a DWARF variable, fall back to the first exported symbol
/// that is a DWARF **function** (`sil_fw_*` control-ABI exports are guaranteed
/// present on every platform and in DWARF, so this cannot come up empty on a
/// working build). This covers ELF/LTO images whose exported data statics never
/// reach the `.dynsym` even though they are in DWARF. Any such symbol works —
/// only the address delta matters, never the value.
fn derive_anchor(bytes: &[u8], dwarf: &DwarfMap) -> Result<(String, AnchorKind), Box<dyn Error>> {
    let object = object::File::parse(bytes)?;
    let exports = object.exports()?;
    let names = || {
        exports
            .iter()
            .filter_map(|e| std::str::from_utf8(e.name()).ok())
    };
    select_anchor(names(), dwarf).ok_or_else(|| {
        format!(
            "no usable ASLR anchor: no exported symbol matches a DWARF variable or function \
             ({} exports, {} DWARF variables, {} DWARF functions)",
            exports.len(),
            dwarf.var_count(),
            dwarf.func_count(),
        )
        .into()
    })
}

/// The anchor-selection strategy order, factored out of [`derive_anchor`] so it is
/// testable without a real object image: try every export against DWARF variables
/// first, then (only if none matched) against DWARF functions. Returns the chosen
/// symbol name + which DWARF map holds its link address.
///
/// **Name normalization:** each export name is matched directly, then with ONE
/// leading underscore stripped. Mach-O decorates every C symbol with a leading
/// underscore in the export table (`_sil_fw_start`) while DWARF holds the source
/// name (`sil_fw_start`), so a direct match can never happen there. Unconditional
/// (not cfg-gated): a stripped-name match is unambiguous and harmless on PE/ELF,
/// whose C exports are undecorated. On a stripped match the returned name is the
/// **stripped (DWARF) name** — also the correct name for the runtime lookup,
/// because `dlsym` on macOS takes the undecorated name and adds the underscore
/// itself.
fn select_anchor<'a>(
    export_names: impl Iterator<Item = &'a str> + Clone,
    dwarf: &DwarfMap,
) -> Option<(String, AnchorKind)> {
    // The candidate DWARF names one export offers: itself, then (if decorated)
    // the Mach-O underscore-stripped form.
    let candidates = |name: &'a str| [Some(name), name.strip_prefix('_')].into_iter().flatten();
    for name in export_names.clone().flat_map(candidates) {
        if dwarf.var_addr(name).is_some() {
            return Some((name.to_string(), AnchorKind::Var));
        }
    }
    for name in export_names.flat_map(candidates) {
        if dwarf.func_addr(name).is_some() {
            return Some((name.to_string(), AnchorKind::Func));
        }
    }
    None
}

/// A firmware instance as a [`Member`]. Borrows a concrete [`Firmware`] and drives
/// it through the internal `Backend` seam on the sim clock; it is the **only** thing
/// that touches firmware memory (routes are table-mediated). The instance `name` is
/// explicit, **not** derived from the DLL, so two boards can run one image as
/// distinct members.
///
/// Each [`advance`](Member::advance) accumulates sim time and, per full firmware-tick
/// period, runs **three fixed phases** over its signal [`Binding`]s (ports are
/// Signals, cvars are Signals, a future transport will be too). Each binding
/// contributes an optional in-sync and/or out-sync half; the sequence never
/// restructures as new `sig_type`s arrive:
///
/// 1. **In-sync (table → firmware)**, each binding's inbound half:
///    - *Ports* — apply pending registrations, then fill every port's input cache
///      from its entry (never driven → `None` → `readSignal` false → driver falls back).
///    - *Cvars (flush)* — write the **fresh** cvars (command-dirtied
///      [`take_dirty`](StateTable::take_dirty)) into memory. Single-threaded ⇒ an entry
///      differs from memory iff command-written, so "flush fresh" ≡ "flush all", done
///      sparsely.
/// 2. **Tick** — `advance_tick` runs the firmware to quiescence (C reads input caches,
///    buffers `writeSignal` output).
/// 3. **Out-sync (firmware → table)**, each binding's outbound half:
///    - *Ports* — drain the output buffer into each port's entry.
///    - *Cvars (sweep)* — read **every** registered leaf out of memory and
///      [`record_mirror`](StateTable::record_mirror) it, so the cvar namespace is an
///      automatic mirror (through pre-resolved handles — no per-tick DWARF).
///
/// **Direction is a property of the binding mechanism, not of a Signal** — the table
/// is the rendezvous, no direction metadata on a signal. A future transport `sig_type`
/// is just a new [`Binding`] variant; the three phases are unchanged. Port I/O is
/// cache-mediated with **no mid-tick State Table access from C**.
///
/// The cvar leaf list is enumerated + registered at enable (whole namespace minus the
/// exclusion policy — [`exclude`](Self::exclude) / [`include`](Self::include));
/// **pending ports are applied there too**, so ports registered during `sil_fw_start`
/// become entries as soon as the member is added. Lifecycle stays on the [`Firmware`]
/// handle (FUTURE: re-enable would reload the DLL — not implemented).
///
/// [`RouteTable`]: crate::route::RouteTable
pub struct FirmwareMember<'b> {
    name: String,
    backend: &'b dyn Backend,
    tick_period_us: u64,
    /// Sim time accumulated toward the next firmware tick.
    accum_us: u64,
    /// The canonical cvar leaf list `(id, read token)`, enumerated + cached at enable
    /// (whole namespace minus excludes). The id's `name` segment is the DWARF path;
    /// the Tier-1 sweep structures below are derived from this.
    cvar_leaves: Vec<(SignalId, CvarRead)>,
    /// **Tier-1 shadow sweep** — resolved (fast-read) leaves, sorted by address,
    /// grouped into `ranges`. The per-tick out-sync `memcmp`s live memory against
    /// each range's shadow and re-decodes only the leaves in changed chunks, making
    /// the sweep O(changed bytes) instead of O(leaves).
    resolved: Vec<SweptLeaf>,
    /// Contiguous memory ranges over [`resolved`](Self::resolved), each with its own
    /// shadow buffer + chunk→leaf map.
    ranges: Vec<ShadowRange>,
    /// Reusable scratch buffer (sized to the largest range) the sweep reads live
    /// memory into before comparing — avoids a per-tick allocation.
    scratch: Vec<u8>,
    /// Per-`resolved`-leaf "decoded this sweep" stamp (deduping a leaf that spans
    /// two changed chunks), compared against [`sweep_gen`](Self::sweep_gen).
    visit_stamp: Vec<u32>,
    /// Monotonic sweep counter stamped into [`visit_stamp`](Self::visit_stamp).
    sweep_gen: u32,
    /// First sweep after (re)enable treats the whole shadow as changed (cold), so
    /// the table gets a full baseline.
    shadow_cold: bool,
    /// Leaves read via the **string path** (a backend that yields no fast-read
    /// handles, e.g. a test mock) — swept one-by-one, outside the shadow machinery.
    path_leaves: Vec<PathLeaf>,
    /// Whether [`cvar_leaves`](Self::cvar_leaves) has been enumerated (guards the
    /// one-time enumeration across re-enables — re-enable only re-registers).
    leaves_cached: bool,
    /// Array-size exclusion threshold for enumeration (default
    /// [`DEFAULT_ARRAY_THRESHOLD`]): arrays larger than this are skipped whole.
    array_threshold: usize,
    /// Prefix drops applied after enumeration (leaves whose path starts with a
    /// prefix are excluded). See [`exclude`](Self::exclude).
    excludes: Vec<String>,
    /// Force-includes: paths/prefixes pulled in despite the array threshold. See
    /// [`include`](Self::include).
    includes: Vec<String>,
    /// Ports this member has applied to the table, in registration order
    /// (deterministic iteration).
    ports: Vec<PortBinding>,
    /// How many of the backend's port defs this member has consumed.
    port_cursor: usize,
}

/// One resolved cvar leaf carried by the Tier-1 shadow sweep: where it lives in
/// firmware memory, how to read it, and where it records into the table.
struct SweptLeaf {
    /// Dense State Table index (resolved once at registration — no per-tick hash).
    table_idx: usize,
    /// Backend fast-read token (decodes the leaf's value).
    handle: CvarHandle,
    /// Runtime address (for range grouping / chunk mapping).
    addr: u64,
    /// Byte size in memory.
    size: usize,
}

/// A contiguous span of firmware memory covering one or more resolved leaves,
/// mirrored by a `shadow` byte buffer. Each tick the sweep `memcmp`s live memory
/// against `shadow`; every changed [`SHADOW_CHUNK`]-byte chunk re-decodes exactly
/// the leaves overlapping it.
struct ShadowRange {
    /// Runtime address of the first byte the shadow mirrors.
    base: u64,
    /// The last-seen bytes of `[base, base+shadow.len())` — mirrors MEMORY, not the
    /// table (updated even where the table dedups the record).
    shadow: Vec<u8>,
    /// Per chunk, the `resolved`-indices of leaves overlapping that chunk (a leaf
    /// straddling a chunk edge appears in every chunk it touches).
    chunk_leaves: Vec<Vec<usize>>,
}

/// A cvar leaf swept via the string-path fallback (no fast-read handle).
struct PathLeaf {
    table_idx: usize,
    path: String,
}

/// One port a firmware member has applied to the table.
struct PortBinding {
    /// C-side handle (index into the backend's port state).
    handle: i32,
    /// Dense State Table index (resolved once — no per-tick hash on the cache fill).
    table_idx: usize,
}

/// How a swept cvar leaf is read each tick: a pre-resolved backend fast-read
/// handle (the hot path), or the string DWARF path (fallback for a backend that
/// yields no handles, e.g. a test mock).
enum CvarRead {
    Resolved(CvarHandle),
    Path(String),
}

/// Tier-1 shadow-sweep chunk size (bytes). A range's shadow is compared against
/// live memory one chunk at a time to localize which leaves changed; 64 B matches
/// a cache line and keeps each range's chunk→leaf map small.
const SHADOW_CHUNK: usize = 64;
/// Max gap (bytes) between two consecutive resolved leaves still merged into one
/// contiguous shadow range. Small enough that the large unmirrored buffers sitting
/// between statics break the chain (so a range never spans them), large enough that
/// a struct's traced fields — separated only by padding or untraced members — share
/// one range.
const SHADOW_MERGE_GAP: u64 = 64;

/// Default array-element-count exclusion threshold (owner default): any array
/// with more than this many elements is excluded whole from the cvar mirror. This
/// naturally drops FreeRTOS task stacks, `ucHeap`, and large scratch buffers,
/// while keeping small per-channel arrays. Override with
/// [`FirmwareMember::set_array_threshold`].
pub const DEFAULT_ARRAY_THRESHOLD: usize = 32;

impl<'b> FirmwareMember<'b> {
    /// Wrap a concrete [`Firmware`] as a member named `name`, advancing one
    /// firmware tick per `tick_period_us` of sim time. The driver keeps its own
    /// `&fw` alongside for ad-hoc white-box access; the member borrows it for the
    /// engine.
    pub fn new(name: &str, fw: &'b Firmware, tick_period_us: u64) -> Self {
        Self::with_backend(name, fw, tick_period_us)
    }

    /// Wrap any [`Backend`] (in-crate mock-backend test seam) as a member. The
    /// public constructor is [`new`](Self::new), which takes the concrete
    /// [`Firmware`]; this exists so unit tests can drive a `FirmwareMember` over
    /// a pure-Rust mock without a firmware DLL.
    pub(crate) fn with_backend(name: &str, backend: &'b dyn Backend, tick_period_us: u64) -> Self {
        Self {
            name: name.to_string(),
            backend,
            tick_period_us,
            accum_us: 0,
            cvar_leaves: Vec::new(),
            resolved: Vec::new(),
            ranges: Vec::new(),
            scratch: Vec::new(),
            visit_stamp: Vec::new(),
            sweep_gen: 0,
            shadow_cold: true,
            path_leaves: Vec::new(),
            leaves_cached: false,
            array_threshold: DEFAULT_ARRAY_THRESHOLD,
            excludes: Vec::new(),
            includes: Vec::new(),
            ports: Vec::new(),
            port_cursor: 0,
        }
    }

    /// **Exclude** every enumerated cvar leaf whose path starts with `prefix` from
    /// the mirror (prefix match only — no globs). Configure before adding the
    /// member (the leaf list is enumerated at enable). Use to drop a noisy or
    /// irrelevant subtree; the array-size threshold already drops stacks/heap.
    pub fn exclude(&mut self, prefix: &str) {
        self.excludes.push(prefix.to_string());
    }

    /// **Force-include** a cvar path (or prefix) that the array-size threshold
    /// would otherwise exclude — e.g. one element of an oversized buffer the test
    /// drives (`HW_SPI_data.channels[0].injectedRx[0]`). A concrete leaf path pulls
    /// in just that element; a prefix naming a whole over-threshold array pulls in
    /// all of it. Prefix match only. Configure before adding the member.
    pub fn include(&mut self, path: &str) {
        self.includes.push(path.to_string());
    }

    /// Override the array-size exclusion threshold (default
    /// [`DEFAULT_ARRAY_THRESHOLD`]). Configure before adding the member.
    pub fn set_array_threshold(&mut self, n: usize) {
        self.array_threshold = n;
    }

    /// How many cvar leaves this member mirrors (0 until enabled). The per-tick
    /// sweep cost is proportional to this count.
    pub fn cvar_leaf_count(&self) -> usize {
        self.cvar_leaves.len()
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
                    Ok(()) => {
                        // Resolve the dense table index once, so the per-tick input
                        // cache fill never hashes the id.
                        let table_idx = st.resolve_index(&id).expect("just registered");
                        self.ports.push(PortBinding {
                            handle: def.handle,
                            table_idx,
                        });
                    }
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

    // --- binding halves: the per-mechanism in-sync / out-sync operations the
    //     three-phase `advance` loop dispatches (see [`Binding`]). Each is one
    //     half of one binding; a binding may contribute an in half, an out half,
    //     or both. Direction lives here on the mechanism, never on a Signal.

    /// Ports in-sync: apply pending registrations, then fill **every** port's
    /// input cache from its entry's current table value (never driven / a
    /// non-numeric value → `None`, which the firmware's `readSignal` reports as
    /// false → the driver falls back). Registrations MUST apply before the cache
    /// fill, so both live in this one half.
    fn in_sync_ports(&mut self, st: &mut StateTable) {
        self.apply_pending_ports(st);
        for port in &self.ports {
            let v = st.current_value_at(port.table_idx).as_ref().and_then(value_to_f64);
            self.backend.set_port_input(port.handle, v);
        }
    }

    /// Cvar in-sync (flush): write the **fresh** `cvar`s in this member's namespace
    /// into firmware memory — command-dirtied ([`take_dirty`](StateTable::take_dirty)).
    /// The production path a route takes to reach memory. Single-threaded ⇒ flushing
    /// fresh ≡ flushing all, done sparsely; an `Err` is a wiring bug, logged.
    fn in_sync_cvars(&mut self, st: &mut StateTable) {
        // Fresh (command-dirtied) cvars in my namespace.
        let flush: Vec<SignalId> = st
            .take_dirty(&self.name)
            .into_iter()
            .filter(|id| id.sig_type() == "cvar")
            .collect();
        for id in flush {
            // The flush set is sparse (command-dirtied), so resolving each id's index
            // here is cheap; `current_value_at` then avoids re-hashing.
            match st.resolve_index(&id) {
                Some(idx) => {
                    if let Some(v) = st.current_value_at(idx) {
                        self.backend.write_cvar(id.name(), &v);
                    }
                }
                None => st.log(
                    LogLevel::Warning,
                    &self.name,
                    format!("cvar flush {id} failed: unknown signal"),
                ),
            }
        }
    }

    /// Ports out-sync: drain the firmware's `writeSignal` output buffer into the
    /// historian, recording each value into its port's entry. A write to a handle
    /// this member never applied is dropped with a Warning.
    fn out_sync_ports(&mut self, st: &mut StateTable) {
        for (handle, value) in self.backend.drain_port_writes() {
            match self.ports.iter().find(|p| p.handle == handle) {
                Some(port) => {
                    let ti = port.table_idx;
                    // A type mismatch here is a bug (a port always writes F64); log it
                    // loud as a Warning, like the other record-error paths.
                    if let Err(e) = st.record_at(ti, Value::F64(value)) {
                        st.log(LogLevel::Warning, &self.name, format!("port record failed: {e}"));
                    }
                }
                None => st.log(
                    LogLevel::Warning,
                    &self.name,
                    format!("port write to unapplied handle {handle} dropped"),
                ),
            }
        }
    }

    /// Cvar out-sync (sweep): mirror firmware memory into the table — the automatic
    /// whole-namespace mirror, made **O(changed bytes)** by the Tier-1 shadow.
    ///
    /// Per contiguous range: `read_range` copies live memory and `memcmp`s it against
    /// the shadow; an unchanged range is skipped. On a mismatch, each changed
    /// [`SHADOW_CHUNK`]-byte chunk re-decodes exactly its overlapping leaves and records
    /// them by dense index ([`record_mirror_at`](StateTable::record_mirror_at) — no
    /// hash, no dirty mark). The shadow mirrors MEMORY (not the table), so it advances
    /// even where the table dedups the record.
    /// The first sweep after (re)enable is **cold** (every chunk changed) for a full
    /// baseline. String-path leaves (a handle-less backend, e.g. a mock) fall back to
    /// the per-leaf read/record loop.
    fn out_sync_cvars(&mut self, st: &mut StateTable) {
        // Fallback: leaves with no resolved layout (string-path backends).
        for pl in &self.path_leaves {
            let v = self.backend.read_cvar(&pl.path);
            if let Err(e) = st.record_mirror_at(pl.table_idx, v) {
                st.log(LogLevel::Warning, &self.name, format!("cvar mirror record failed: {e}"));
            }
        }

        if self.ranges.is_empty() {
            return;
        }
        self.sweep_gen = self.sweep_gen.wrapping_add(1);
        let gen = self.sweep_gen;
        let cold = self.shadow_cold;
        let backend = self.backend;
        // Move the reusable buffers out so the range loop can hold a `&mut` to
        // `self.ranges` while still touching scratch/visit_stamp/resolved.
        let mut scratch = std::mem::take(&mut self.scratch);
        let mut stamp = std::mem::take(&mut self.visit_stamp);
        let resolved = &self.resolved;

        for range in &mut self.ranges {
            let len = range.shadow.len();
            // Range-level fast path: unchanged whole range → skip with a bare
            // memcmp against live memory (no copy). Most ranges are unchanged each
            // tick, so this is the common case.
            if !cold && backend.range_eq(range.base, &range.shadow) {
                continue;
            }
            // Changed (or cold): pull the live bytes in and localize per chunk.
            let buf = &mut scratch[..len];
            backend.read_range(range.base, buf);
            let nchunks = range.chunk_leaves.len();
            for c in 0..nchunks {
                let cs = c * SHADOW_CHUNK;
                let ce = (cs + SHADOW_CHUNK).min(len);
                if cold || buf[cs..ce] != range.shadow[cs..ce] {
                    for &li in &range.chunk_leaves[c] {
                        // Dedup a leaf that straddles two changed chunks.
                        if stamp[li] != gen {
                            stamp[li] = gen;
                            let leaf = &resolved[li];
                            // Typed decode → typed fast lane (scalars record natively;
                            // enum/other ride the boxed lane).
                            let idx = leaf.table_idx;
                            // A wrong-kind record is a bug (a static's C type is fixed):
                            // log a Warning rather than abort the sweep.
                            let res = match backend.read_cvar_scalar(leaf.handle) {
                                ScalarSample::F32(x) => st.record_mirror_f32_at(idx, x),
                                ScalarSample::F64(x) => st.record_mirror_f64_at(idx, x),
                                ScalarSample::I32(x) => st.record_mirror_i32_at(idx, x),
                                ScalarSample::U32(x) => st.record_mirror_u32_at(idx, x),
                                ScalarSample::U64(x) => st.record_mirror_u64_at(idx, x),
                                ScalarSample::Bool(x) => st.record_mirror_bool_at(idx, x),
                                ScalarSample::Boxed(v) => st.record_mirror_at(idx, v),
                            };
                            if let Err(e) = res {
                                st.log(LogLevel::Warning, &self.name, format!("cvar mirror record failed: {e}"));
                            }
                        }
                    }
                    // The shadow mirrors MEMORY: absorb the changed bytes
                    // unconditionally (independent of what the table did).
                    range.shadow[cs..ce].copy_from_slice(&buf[cs..ce]);
                }
            }
        }

        self.scratch = scratch;
        self.visit_stamp = stamp;
        self.shadow_cold = false;
    }
}

/// One **firmware-side signal binding** — a mechanism that mirror-syncs a class of
/// signals between the State Table and firmware memory around a tick. Each contributes
/// an optional **in-sync** half (table → firmware, before `advance_tick`) and/or an
/// **out-sync** half (firmware → table, after). The [`advance`](FirmwareMember::advance)
/// loop is fixed at three phases — **in-sync → tick → out-sync** — and a new `sig_type`
/// is a new variant here, never a new phase.
///
/// **Direction is a property of the mechanism, not of a Signal.** Both `Cvars` and
/// `Ports` are behaviorally in + out — one collective binding each over the backend's
/// state; the point is the phase structure, not one binding per signal.
#[derive(Clone, Copy)]
enum Binding {
    /// Cvars (in + out): flush fresh cvars into memory; sweep the mirror out.
    Cvars,
    /// Ports (in + out): apply registrations + fill input caches; drain writes.
    Ports,
}

impl Binding {
    /// Every binding, in deterministic in-sync/out-sync order. `Ports` precedes
    /// `Cvars` so port registrations + cache fill happen first (matching the
    /// historical sequence; the two are independent — different backend state).
    /// Out-sync runs the same order (`Ports` drain before the `Cvars` sweep).
    const ALL: [Binding; 2] = [Binding::Ports, Binding::Cvars];

    /// Run this binding's in-sync half (table → firmware), if any.
    fn in_sync(self, fm: &mut FirmwareMember, st: &mut StateTable) {
        match self {
            Binding::Ports => fm.in_sync_ports(st),
            Binding::Cvars => fm.in_sync_cvars(st),
        }
    }

    /// Run this binding's out-sync half (firmware → table), if any.
    fn out_sync(self, fm: &mut FirmwareMember, st: &mut StateTable) {
        match self {
            Binding::Ports => fm.out_sync_ports(st),
            Binding::Cvars => fm.out_sync_cvars(st),
        }
    }
}

impl Member for FirmwareMember<'_> {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, dt_us: u64, st: &mut StateTable) {
        // One firmware tick per full period elapsed (usually exactly one). Each runs
        // three fixed phases over every binding (see [`Binding`]); C never touches the table.
        self.accum_us += dt_us;
        while self.accum_us >= self.tick_period_us {
            self.accum_us -= self.tick_period_us;
            // Phase 1 — in-sync: every binding's table → firmware half.
            for binding in Binding::ALL {
                binding.in_sync(self, st);
            }
            // Phase 2 — tick: run the firmware to quiescence.
            self.backend.advance_tick();
            // Phase 3 — out-sync: every binding's firmware → table half.
            for binding in Binding::ALL {
                binding.out_sync(self, st);
            }
        }
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            if !self.leaves_cached {
                self.enumerate_and_register(st);
            } else {
                // Re-enable re-registers the cached leaves idempotently (no
                // re-enumerate) and forces a cold sweep to re-baseline the table.
                for (id, _) in &self.cvar_leaves {
                    let _ = st.register(id.clone(), None);
                }
                self.shadow_cold = true;
            }
            // Ports registered before this member existed (during `sil_fw_start`)
            // become table entries here — immediately at add_member — so routes into
            // them are valid from the first step. Later ones apply at next advance.
            self.apply_pending_ports(st);
        }
        // FUTURE: reload the DLL (boot-from-reset) on re-enable; today enable only
        // gates advance (the engine skips a disabled member's tick).
    }
}

impl FirmwareMember<'_> {
    /// Enumerate the firmware's traceable cvar leaves once (exclusion policy +
    /// includes applied), register each under this member's namespace, and cache
    /// the resolved leaf list for the sweep. Reports the registered-leaf count (and
    /// what the policy dropped) as an Info log so the number is visible.
    fn enumerate_and_register(&mut self, st: &mut StateTable) {
        let en = self.backend.enumerate_cvars(self.array_threshold, &self.includes);
        for (path, handle) in en.leaves {
            if self.excludes.iter().any(|ex| path.starts_with(ex)) {
                continue;
            }
            match SignalId::new("cvar", &self.name, &path, None) {
                Ok(id) => match st.register(id.clone(), None) {
                    Ok(()) => {
                        let read = match handle {
                            Some(h) => CvarRead::Resolved(h),
                            None => CvarRead::Path(path),
                        };
                        self.cvar_leaves.push((id, read));
                    }
                    Err(e) => st.log(
                        LogLevel::Warning,
                        &self.name,
                        format!("cvar {id} register failed: {e}"),
                    ),
                },
                Err(e) => st.log(
                    LogLevel::Warning,
                    &self.name,
                    format!("cvar path {path:?} yields an invalid signal id: {e}"),
                ),
            }
        }
        self.leaves_cached = true;
        // Build the Tier-1 shadow-sweep structures from the freshly-registered
        // leaves (resolve indices, group into ranges, allocate shadows).
        self.build_shadow(st);
        st.log(
            LogLevel::Info,
            &self.name,
            format!(
                "auto-registered {} cvar leaves ({} arrays excluded, {} leaves skipped{})",
                self.cvar_leaves.len(),
                en.excluded_arrays,
                en.skipped_leaves,
                if en.capped {
                    ", SAFETY CAP HIT"
                } else {
                    ""
                }
            ),
        );
    }

    /// Build the Tier-1 shadow-sweep structures from the enumerated cvar leaves:
    /// resolve each leaf's dense table index; split fast-read (resolved) leaves from
    /// string-path leaves; sort the resolved by address; group them into contiguous
    /// ranges (merging across gaps ≤ [`SHADOW_MERGE_GAP`]); and allocate the shadow
    /// and scratch buffers. Marks the shadow cold so the first sweep baselines the
    /// table.
    fn build_shadow(&mut self, st: &StateTable) {
        let backend = self.backend;
        let mut resolved: Vec<SweptLeaf> = Vec::new();
        let mut path: Vec<PathLeaf> = Vec::new();
        for (id, read) in &self.cvar_leaves {
            let table_idx = st.resolve_index(id).expect("leaf just registered");
            match read {
                CvarRead::Resolved(h) => {
                    let (addr, size) = backend.cvar_layout(*h);
                    resolved.push(SweptLeaf {
                        table_idx,
                        handle: *h,
                        addr,
                        size,
                    });
                }
                CvarRead::Path(p) => path.push(PathLeaf {
                    table_idx,
                    path: p.clone(),
                }),
            }
        }
        resolved.sort_by_key(|l| l.addr);
        let ranges = build_ranges(&resolved);
        let scratch_len = ranges.iter().map(|r| r.shadow.len()).max().unwrap_or(0);
        self.visit_stamp = vec![0u32; resolved.len()];
        self.scratch = vec![0u8; scratch_len];
        self.resolved = resolved;
        self.ranges = ranges;
        self.path_leaves = path;
        self.shadow_cold = true;
    }
}

/// Group address-sorted resolved leaves into contiguous shadow ranges: a new range
/// starts whenever the next leaf begins more than [`SHADOW_MERGE_GAP`] bytes past
/// the current range's end. Each range records, per [`SHADOW_CHUNK`]-byte chunk, the
/// indices (into `leaves`) of the leaves overlapping that chunk — a leaf straddling
/// a chunk edge is listed under every chunk it touches. Deterministic: input is
/// address-sorted, output is in ascending address order.
fn build_ranges(leaves: &[SweptLeaf]) -> Vec<ShadowRange> {
    let mut ranges: Vec<ShadowRange> = Vec::new();
    let mut i = 0;
    while i < leaves.len() {
        let start = leaves[i].addr;
        let mut end = leaves[i].addr + leaves[i].size as u64;
        let mut j = i + 1;
        while (j < leaves.len()) && (leaves[j].addr <= (end + SHADOW_MERGE_GAP)) {
            end = end.max(leaves[j].addr + (leaves[j].size as u64));
            j += 1;
        }
        let span = (end - start) as usize;
        let nchunks = span.div_ceil(SHADOW_CHUNK);
        let mut chunk_leaves: Vec<Vec<usize>> = vec![Vec::new(); nchunks];
        for (k, leaf) in leaves.iter().enumerate().take(j).skip(i) {
            let first = ((leaf.addr - start) as usize) / SHADOW_CHUNK;
            let last = (((leaf.addr + (leaf.size as u64) - 1) - start) as usize) / SHADOW_CHUNK;
            for cl in chunk_leaves.iter_mut().take(last + 1).skip(first) {
                cl.push(k);
            }
        }
        ranges.push(ShadowRange {
            base: start,
            shadow: vec![0u8; span],
            chunk_leaves,
        });
        i = j;
    }
    ranges
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

/// The byte width of a firmware [`Scalar`] leaf — the size the shadow sweep reads
/// and compares for that leaf.
fn scalar_byte_size(kind: Scalar) -> usize {
    match kind {
        Scalar::U8 | Scalar::I8 | Scalar::Bool => 1,
        Scalar::U16 | Scalar::I16 => 2,
        Scalar::U32 | Scalar::I32 | Scalar::F32 => 4,
        Scalar::U64 | Scalar::I64 | Scalar::F64 => 8,
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

/// Read a firmware scalar as a native [`ScalarSample`] — the typed-decode twin of
/// [`scalar_to_value`], with the identical coercion table (signed widths → `I32`,
/// unsigned → `U32`/`U64`, `I64` narrows to `I32`). Feeds the State Table's typed
/// mirror fast lanes so a changing scalar leaf never constructs a `Value`.
///
/// SAFETY: `p` points at a readable value of `kind`'s size; firmware quiescent.
unsafe fn scalar_to_sample(p: *const u8, kind: Scalar) -> ScalarSample {
    match kind {
        Scalar::U8 => ScalarSample::U32(p.read_unaligned() as u32),
        Scalar::U16 => ScalarSample::U32((p as *const u16).read_unaligned() as u32),
        Scalar::U32 => ScalarSample::U32((p as *const u32).read_unaligned()),
        Scalar::U64 => ScalarSample::U64((p as *const u64).read_unaligned()),
        Scalar::I8 => ScalarSample::I32((p as *const i8).read_unaligned() as i32),
        Scalar::I16 => ScalarSample::I32((p as *const i16).read_unaligned() as i32),
        Scalar::I32 => ScalarSample::I32((p as *const i32).read_unaligned()),
        Scalar::I64 => ScalarSample::I32((p as *const i64).read_unaligned() as i32),
        Scalar::F32 => ScalarSample::F32((p as *const f32).read_unaligned()),
        Scalar::F64 => ScalarSample::F64((p as *const f64).read_unaligned()),
        Scalar::Bool => ScalarSample::Bool(p.read_unaligned() != 0),
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

    /// A pure-Rust [`Backend`] with no DLL: a configurable cvar "memory" plus an
    /// enumerable leaf list, used to prove the member/mirror semantics without a
    /// real firmware image. Logs every `write_cvar` path so a test can assert the
    /// flush is **sparse** (only fresh cvars, not the whole namespace).
    #[derive(Default)]
    struct MockBackend {
        ticks: RefCell<u64>,
        cvars: RefCell<HashMap<String, Value>>,
        leaves: Vec<String>,
        writes: RefCell<Vec<String>>,
    }

    impl MockBackend {
        /// A backend whose enumeration yields exactly `leaves` (each read via the
        /// string-path fallback — the mock hands out no fast-read handles).
        fn with_leaves(leaves: &[&str]) -> Self {
            Self {
                leaves: leaves.iter().map(|s| (*s).to_string()).collect(),
                ..Default::default()
            }
        }
    }

    impl Backend for MockBackend {
        fn advance_tick(&self) {
            *self.ticks.borrow_mut() += 1;
        }
        fn read_cvar(&self, path: &str) -> Value {
            self.cvars
                .borrow()
                .get(path)
                .cloned()
                .unwrap_or(Value::U32(0))
        }
        fn write_cvar(&self, path: &str, v: &Value) {
            self.writes.borrow_mut().push(path.to_string());
            self.cvars.borrow_mut().insert(path.to_string(), v.clone());
        }
        fn enumerate_cvars(&self, _threshold: usize, _includes: &[String]) -> CvarEnumeration {
            CvarEnumeration {
                leaves: self.leaves.iter().map(|p| (p.clone(), None)).collect(),
                ..CvarEnumeration::default()
            }
        }
    }

    fn id(s: &str) -> SignalId {
        SignalId::parse(s).unwrap()
    }

    #[test]
    fn anchor_prefers_variable_then_falls_back_to_function() {
        // Exports as the object crate would yield them; both a data global and the
        // control-ABI function are present in the export table.
        let exports = ["sil_fw_start", "g_var"];

        // Variable present in DWARF -> variable strategy wins (Windows path).
        let dw = DwarfMap::for_anchor_test(&[("g_var", 0x100)], &[("sil_fw_start", 0x200)]);
        let (name, kind) = select_anchor(exports.iter().copied(), &dw).unwrap();
        assert_eq!(name, "g_var");
        assert_eq!(kind, AnchorKind::Var);

        // No exported symbol is a DWARF variable (the ELF/LTO case: data statics in
        // DWARF but absent from .dynsym) -> function fallback picks sil_fw_start.
        let dw = DwarfMap::for_anchor_test(&[("some_internal_static", 0x100)], &[("sil_fw_start", 0x200)]);
        let (name, kind) = select_anchor(exports.iter().copied(), &dw).unwrap();
        assert_eq!(name, "sil_fw_start");
        assert_eq!(kind, AnchorKind::Func);

        // Neither strategy matches -> None (derive_anchor turns this into the
        // count-carrying diagnostic error).
        let dw = DwarfMap::for_anchor_test(&[], &[]);
        assert!(select_anchor(exports.iter().copied(), &dw).is_none());
    }

    #[test]
    fn anchor_strips_macho_leading_underscore() {
        // Mach-O export tables decorate C symbols with a leading underscore
        // (`_g_var`, `_sil_fw_start`) while DWARF holds the source names, so only
        // the stripped form can match. The returned name must be the STRIPPED
        // (DWARF) name — dlsym on macOS expects the undecorated name.
        let exports = ["_sil_fw_start", "_g_var"];

        // Variable strategy through the stripped form.
        let dw = DwarfMap::for_anchor_test(&[("g_var", 0x100)], &[("sil_fw_start", 0x200)]);
        let (name, kind) = select_anchor(exports.iter().copied(), &dw).unwrap();
        assert_eq!(name, "g_var");
        assert_eq!(kind, AnchorKind::Var);

        // Function fallback through the stripped form (the macOS failure mode:
        // no exported name matches a DWARF variable even after stripping).
        let dw = DwarfMap::for_anchor_test(&[], &[("sil_fw_start", 0x200)]);
        let (name, kind) = select_anchor(exports.iter().copied(), &dw).unwrap();
        assert_eq!(name, "sil_fw_start");
        assert_eq!(kind, AnchorKind::Func);

        // A direct (undecorated) match still wins as before — stripping is an
        // additional candidate, not a replacement.
        let dw = DwarfMap::for_anchor_test(&[("_g_var", 0x100)], &[]);
        let (name, kind) = select_anchor(exports.iter().copied(), &dw).unwrap();
        assert_eq!(name, "_g_var");
        assert_eq!(kind, AnchorKind::Var);
    }

    #[test]
    fn backend_is_object_safe_and_usable_via_dyn() {
        let be: Box<dyn Backend> = Box::new(MockBackend::default());
        be.advance_tick();
        be.advance_tick();
        be.write_cvar("x", &Value::U32(42));
        assert_eq!(be.read_cvar("x"), Value::U32(42));
        assert_eq!(be.read_cvar("unset"), Value::U32(0));
    }

    #[test]
    fn firmware_member_auto_mirrors_the_cvar_namespace() {
        // No per-signal declaration: the member enumerates + registers the whole
        // (mock) namespace at enable and sweeps it into the historian each tick.
        let be = MockBackend::with_leaves(&["counter"]);
        be.write_cvar("counter", &Value::U32(7));

        let cid = id("cvar:dut:counter");
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);

        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st); // enumerates + registers cvar:dut:counter
        assert_eq!(fm.cvar_leaf_count(), 1);
        assert_eq!(st.len(), 1);

        // One full period -> one firmware tick + one mirror sweep.
        st.set_time(1_000);
        fm.advance(1_000, &mut st);
        assert_eq!(*be.ticks.borrow(), 1);
        assert_eq!(st.current_value(&cid).unwrap(), Some(Value::U32(7)));

        // A sub-period advance accumulates but does not tick the firmware.
        fm.advance(500, &mut st);
        assert_eq!(*be.ticks.borrow(), 1);
        // The next 500us completes the period -> a second tick.
        fm.advance(500, &mut st);
        assert_eq!(*be.ticks.borrow(), 2);
    }

    #[test]
    fn firmware_member_flushes_fresh_cvars_before_ticking() {
        // The flush side: a command-written (dirty) cvar entry is pushed into
        // firmware memory before advance_tick. A route/test records the entry; the
        // member flushes the fresh id — no per-signal `drive` declaration.
        let be = MockBackend::with_leaves(&["sensor_in"]);
        let sid = id("cvar:dut:sensor_in");
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);

        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st); // auto-registers cvar:dut:sensor_in
        assert_eq!(st.len(), 1);

        // Command a value into the table entry (as a route would), then advance.
        st.set_time(1_000);
        st.record(&sid, Value::U32(42)).unwrap();
        fm.advance(1_000, &mut st);
        assert_eq!(*be.ticks.borrow(), 1);
        assert_eq!(be.read_cvar("sensor_in"), Value::U32(42));

        // Re-command; the next tick flushes the new value.
        st.set_time(2_000);
        st.record(&sid, Value::U32(7)).unwrap();
        fm.advance(1_000, &mut st);
        assert_eq!(be.read_cvar("sensor_in"), Value::U32(7));
    }

    #[test]
    fn flush_is_sparse_only_fresh_ids_written() {
        // The single-threaded flush≡all invariant, proven sparse: with three
        // mirrored leaves, only the ONE command-written entry is flushed; the
        // untouched two are never written back, yet all three still mirror.
        let be = MockBackend::with_leaves(&["a", "b", "c"]);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);

        let a = id("cvar:dut:a");
        st.set_time(1_000);
        st.record(&a, Value::U32(1)).unwrap(); // command only `a`
        be.writes.borrow_mut().clear();
        fm.advance(1_000, &mut st);
        assert_eq!(&*be.writes.borrow(), &["a".to_string()]); // sparse: just `a`
        // The untouched entries still mirror (swept out of memory).
        assert_eq!(st.current_value(&id("cvar:dut:b")).unwrap(), Some(Value::U32(0)));

        // A tick with no fresh command writes nothing at all.
        be.writes.borrow_mut().clear();
        st.set_time(2_000);
        fm.advance(1_000, &mut st);
        assert!(be.writes.borrow().is_empty(), "untouched entries are not flushed");
    }

    #[test]
    fn auto_registration_is_idempotent_across_re_enable() {
        let be = MockBackend::with_leaves(&["x", "y"]);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);
        assert_eq!((st.len(), fm.cvar_leaf_count()), (2, 2));

        // Disable then re-enable: no re-enumeration, no duplicate registration.
        fm.set_enabled(false, &mut st);
        fm.set_enabled(true, &mut st);
        assert_eq!((st.len(), fm.cvar_leaf_count()), (2, 2));
    }

    #[test]
    fn exclude_prefix_drops_a_subtree() {
        let be = MockBackend::with_leaves(&["keep.a", "drop.b", "drop.c"]);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        fm.exclude("drop");
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);
        assert_eq!(fm.cvar_leaf_count(), 1); // only keep.a survives
        assert_eq!(st.current_value(&id("cvar:dut:keep.a")).unwrap(), None);
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
        fn advance_tick(&self) {
            let seen = self.ports.inner.borrow().read(0);
            *self.seen.borrow_mut() = seen;
            if let Some(v) = seen {
                self.ports.inner.borrow_mut().write(1, v * 2.0);
            }
        }
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
        let mut fm = FirmwareMember::with_backend("dut", &mock, 1_000);
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
        assert_eq!(st.current_value(&out_id).unwrap(), Some(Value::F64(3.0)));
    }

    #[test]
    fn undriven_port_reads_as_not_driven() {
        let mock = port_mock_with_in_out();
        let mut fm = FirmwareMember::with_backend("dut", &mock, 1_000);
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
        let mut fm = FirmwareMember::with_backend("dut", &mock, 1_000);
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

    // --- Tier-1 shadow sweep -------------------------------------------------

    /// A backend with a byte-array "firmware memory" and `u32` leaves at fixed
    /// offsets, exposing the shadow-sweep primitives (`cvar_layout`/`range_eq`/
    /// `read_range`/`read_cvar_resolved`). It counts decodes per leaf so a test can
    /// assert *which* leaves the sweep re-decoded. Leaf addresses are byte offsets
    /// into `mem` (the sweep treats them opaquely).
    struct ShadowLeaf {
        path: String,
        off: usize,
    }

    struct ShadowMock {
        mem: RefCell<Vec<u8>>,
        leaves: Vec<ShadowLeaf>,
        decodes: RefCell<Vec<u32>>,
    }

    impl ShadowMock {
        fn new(size: usize, leaves: &[(&str, usize)]) -> Self {
            Self {
                mem: RefCell::new(vec![0u8; size]),
                leaves: leaves
                    .iter()
                    .map(|(p, o)| ShadowLeaf {
                        path: (*p).to_string(),
                        off: *o,
                    })
                    .collect(),
                decodes: RefCell::new(vec![0u32; leaves.len()]),
            }
        }
        fn set_u32(&self, off: usize, v: u32) {
            self.mem.borrow_mut()[off..off + 4].copy_from_slice(&v.to_le_bytes());
        }
        fn set_byte(&self, off: usize, b: u8) {
            self.mem.borrow_mut()[off] = b;
        }
        fn decodes(&self, i: usize) -> u32 {
            self.decodes.borrow()[i]
        }
    }

    impl Backend for ShadowMock {
        fn advance_tick(&self) {}
        fn read_cvar(&self, _path: &str) -> Value {
            Value::U32(0)
        }
        fn write_cvar(&self, _path: &str, _v: &Value) {}
        fn enumerate_cvars(&self, _threshold: usize, _includes: &[String]) -> CvarEnumeration {
            CvarEnumeration {
                leaves: self
                    .leaves
                    .iter()
                    .enumerate()
                    .map(|(i, l)| (l.path.clone(), Some(CvarHandle(i))))
                    .collect(),
                ..CvarEnumeration::default()
            }
        }
        fn read_cvar_resolved(&self, handle: CvarHandle) -> Value {
            self.decodes.borrow_mut()[handle.0] += 1;
            let off = self.leaves[handle.0].off;
            let mem = self.mem.borrow();
            Value::U32(u32::from_le_bytes(mem[off..off + 4].try_into().unwrap()))
        }
        fn cvar_layout(&self, handle: CvarHandle) -> (u64, usize) {
            (self.leaves[handle.0].off as u64, 4)
        }
        fn range_eq(&self, addr: u64, shadow: &[u8]) -> bool {
            let off = addr as usize;
            let mem = self.mem.borrow();
            mem[off..off + shadow.len()] == *shadow
        }
        fn read_range(&self, addr: u64, buf: &mut [u8]) {
            let off = addr as usize;
            let mem = self.mem.borrow();
            buf.copy_from_slice(&mem[off..off + buf.len()]);
        }
    }

    #[test]
    fn shadow_sweep_cold_records_all_then_localizes_a_changed_chunk() {
        // One range [0,76): a@0 (chunk0), b@62 (spans chunk0/chunk1 — 62..66),
        // c@72 (chunk1). Merge gap keeps them in one range; chunk size is 64.
        let be = ShadowMock::new(80, &[("a", 0), ("b", 62), ("c", 72)]);
        be.set_u32(0, 10);
        be.set_u32(62, 20);
        be.set_u32(72, 30);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);

        // Cold sweep: every leaf decoded once and baselined into the table.
        st.set_time(1_000);
        fm.advance(1_000, &mut st);
        assert_eq!((be.decodes(0), be.decodes(1), be.decodes(2)), (1, 1, 1));
        for (p, v) in [("a", 10u32), ("b", 20), ("c", 30)] {
            assert_eq!(
                st.current_value(&id(&format!("cvar:dut:{p}"))).unwrap(),
                Some(Value::U32(v))
            );
        }

        // Change a byte inside chunk0 (part of `a`). Only chunk0's leaves re-decode:
        // a + b (b straddles the chunk edge); c (chunk1 only) is NOT touched.
        be.set_u32(0, 11);
        st.set_time(2_000);
        fm.advance(1_000, &mut st);
        assert_eq!((be.decodes(0), be.decodes(1), be.decodes(2)), (2, 2, 1));
        // a changed -> re-recorded; b decoded but unchanged -> historian did not grow.
        assert_eq!(st.current_value(&id("cvar:dut:a")).unwrap(), Some(Value::U32(11)));
        assert_eq!(st.changes(&id("cvar:dut:b")).unwrap().len(), 1);
    }

    #[test]
    fn shadow_sweep_boundary_leaf_caught_from_its_far_chunk() {
        // b@62 spans chunk0 (62,63) and chunk1 (64,65). Change ONLY a chunk1 byte
        // of b: chunk0 stays equal, chunk1 changes -> b (via chunk1) and c decode,
        // a (chunk0 only) does not. Proves a boundary leaf is caught from either
        // chunk it overlaps.
        let be = ShadowMock::new(80, &[("a", 0), ("b", 62), ("c", 72)]);
        be.set_u32(0, 10);
        be.set_u32(62, 20);
        be.set_u32(72, 30);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);
        st.set_time(1_000);
        fm.advance(1_000, &mut st); // cold: (1,1,1)

        be.set_byte(65, 0xAA); // chunk1 byte of b only
        st.set_time(2_000);
        fm.advance(1_000, &mut st);
        assert_eq!((be.decodes(0), be.decodes(1), be.decodes(2)), (1, 2, 2));
        // b's value changed -> re-recorded (two change-log entries now).
        assert_eq!(st.changes(&id("cvar:dut:b")).unwrap().len(), 2);
    }

    #[test]
    fn shadow_sweep_splits_ranges_across_large_gaps() {
        // a@0 and b@200 are farther apart than the merge gap -> two independent
        // ranges. A change in a's range must not re-decode b.
        let be = ShadowMock::new(256, &[("a", 0), ("b", 200)]);
        be.set_u32(0, 1);
        be.set_u32(200, 2);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);
        st.set_time(1_000);
        fm.advance(1_000, &mut st); // cold: both decoded once

        be.set_u32(0, 5); // touch only a's range
        st.set_time(2_000);
        fm.advance(1_000, &mut st);
        assert_eq!(be.decodes(0), 2);
        assert_eq!(be.decodes(1), 1); // b's range untouched -> not decoded
    }

    #[test]
    fn shadow_decodes_once_per_memory_change() {
        // The shadow tracks MEMORY: a leaf decodes exactly once per memory change (the
        // tick it changed) and not again on a subsequent unchanged tick — the shadow
        // absorbed the change.
        let be = ShadowMock::new(16, &[("a", 0)]);
        be.set_u32(0, 10);
        let mut fm = FirmwareMember::with_backend("dut", &be, 1_000);
        let mut st = StateTable::new();
        fm.set_enabled(true, &mut st);
        let a = id("cvar:dut:a");
        st.set_time(1_000);
        fm.advance(1_000, &mut st); // cold: a=10, decode=1
        assert_eq!(be.decodes(0), 1);
        assert_eq!(st.current_value(&a).unwrap(), Some(Value::U32(10)));

        be.set_u32(0, 77); // memory changes
        st.set_time(2_000);
        fm.advance(1_000, &mut st);
        assert_eq!(be.decodes(0), 2); // chunk changed -> decoded
        assert_eq!(st.current_value(&a).unwrap(), Some(Value::U32(77))); // mirror follows

        // Memory now stable: the shadow was updated to 77, so no re-decode.
        st.set_time(3_000);
        fm.advance(1_000, &mut st);
        assert_eq!(be.decodes(0), 2);
    }
}
