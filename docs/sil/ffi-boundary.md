# D2 — Rust ↔ C FFI boundary (resolved design)

Resolves open decision **D2** from [`architecture.md`](architecture.md): how
the Rust framework drives and inspects the native firmware.

**Decision:** **in-process**, one firmware instance per process, with the
firmware built as a **dynamically loaded shared library**. All firmware *data*
is read/written directly in-process by address via **DWARF symbol
introspection** — there are **no sim-specific getter/setter functions in the
firmware**. That introspection is the substrate for the framework's **State
Table**; data movement between firmware and models is expressed as routes over
that table (see [`state-route-tables.md`](state-route-tables.md)). The only
hand-written C surface is a tiny **control ABI** (hook installation, lifecycle,
timebase advance, ISR dispatch), plus a small set of **C→Rust upcalls** the sim
HW layer uses to register interrupts (D8, §6). Fast-mode parallelism is process-level (`pytest -n`).

---

## 1. In-process, one instance per process

The firmware is a large bag of C global state — the FreeRTOS kernel, task
stacks, `static HW_ADC_data`, etc. Two independent firmware instances cannot
coexist in one OS process. That single constraint settles the model:

- **In-process:** Rust framework + plant models + firmware share one process
  **and one thread** — the D1 fiber model runs the firmware as fibers in the
  framework's thread; "advance" is a fiber swap, not cross-thread sync
  ([`freertos-tick.md`](freertos-tick.md), [`performance.md`](performance.md)).
- **Parallelism is process-level:** `pytest -n` / xdist spawns N worker
  processes, each loading its own firmware copy. Free crash isolation (one
  worker segfaults, the rest survive).
- **Subprocess + IPC rejected:** it buys only crash isolation (which we get at
  the worker-process level anyway) while making white-box access cross-process
  (ptrace / ReadProcessMemory) and turning every per-tick step into an IPC
  round-trip. In-process wins decisively for our goals.

## 2. Dynamic load, not static link

The firmware builds as a **shared library** (`.dll` / `.dylib`) that the Rust
harness `LoadLibrary` / `dlopen`s at runtime.

| | Dynamic load (chosen) | Static link |
|---|---|---|
| Iteration | rebuild fw, reload — **no Rust relink** | fw change → relink Rust |
| DWARF | firmware-only, clean to parse | mixed Rust+C image, messier |
| Artifact framing | "load the built artifact" ✓ | — |
| Instances/process | 1 | 1 |

Single-instance-per-process holds either way (statics are process-global), and
we already accepted that, so dynamic load's iteration + introspection wins
carry the decision.

## 3. The control ABI (the only hand-written C surface)

Lifecycle + the per-step advance. This is **control, not data** — you cannot
"advance the scheduler" by poking a variable, so these stay functions.
`dispatch_isr` internally swaps into the firmware fibers and returns when the
firmware yields to quiescence (D1), so Rust sees a plain synchronous call and the
fiber machinery stays hidden in the port layer.

```c
// sil_fw.h — the stable Rust<->C surface
void sil_fw_setHooks(const SIL_ports_hooks_S *hooks);  // port seam, BEFORE start
void sil_fw_setIrqHooks(const SIL_irq_hooks_S *hooks); // interrupt seam, BEFORE start
bool sil_fw_start(void);                        // HW init + tasks + scheduler to first quiescence
void sil_fw_advance_time(uint32_t elapsed_us);  // move the hardware timebase; runs no firmware
bool sil_fw_dispatch_isr(SIL_irq_handler_F h);  // run one handler in the ISR bracket (D8)
void sil_fw_shutdown(void);
```

The two installers gate execution rather than decorate it: with no interrupt
hooks installed nothing registers a handler, so nothing is ever due and no
firmware advances. Both are called before `sil_fw_start`, both copy the struct,
and both are null-safe on the C side, so a hookless standalone or Unity run
behaves exactly as it did before the seam existed.

Firmware execution is entirely interrupt-driven, the kernel tick included: the
port registers its systick with the framework's interrupt table at scheduler
start ([`sim-interrupts.md`](sim-interrupts.md)) and it arrives back through
`sil_fw_dispatch_isr` like any other handler.

`sil_fw_start` returns `false` on init/task-creation failure (the framework
reports it — the firmware never calls `Error_Handler` in SIL). **Pacing is the
driver's choice:** because the driver *drives* the step (rather than the port
running its own tick loop), realtime-vs-fast is just whether the caller paces to
wall-clock or runs flat out — the firmware exposes only the per-step
primitives. There are **no sim-specific data functions** beyond this; the firmware
is otherwise unaware it is being simulated. (The Rust framework is the driver;
`sw/fw/src/main.c`'s SIM `main()` is a boot smoke check only.)

## 4. DWARF introspection — the State Table substrate

All firmware data access is by **direct in-process memory access**, located
via debug info. This is what makes the State Table ([`state-route-tables.md`](state-route-tables.md))
able to hold an entry for *every* firmware static with zero firmware changes.

- Native lib is built with `-g` and `-O0` (already in `native.cmake`). **-O0
  matters**: every `static` (including function-local statics) keeps a real,
  stable address and isn't elided, so the whole set is enumerable and
  addressable. The optimized (`-O3`) SIL flavor mostly preserves this, with one
  wrinkle the reader handles: SRA can decompose an aggregate static into a
  `DW_OP_piece` composite location (some members folded away entirely) — live
  members still resolve through their piece addresses; folded ones don't.
- Parse the lib once with the `object` (ELF/Mach-O/PE) + `gimli` (DWARF)
  crates to build a map: `name → { link location, type }` (a location is a
  whole-object address or a piece list). DWARF gives struct layout, enums,
  arrays — so nested members flatten to addressable leaves
  (`HW_ADC_data.channelData[0].counts[3]` = base + offset).
- In-process, a global lives in our own address space — reading/writing it is a
  pointer dereference, **no ptrace / no cross-process memory API**.
- Resolve ASLR once: `dlsym` a known **anchor symbol**, compare its runtime
  address to its link address → the load slide → apply to every entry's link
  address to get its live address.

Typed access (`read::<f32>(...)` / `write::<u32>(...)`) sits behind a safe Rust
wrapper that checks the requested Rust type against the entry's DWARF type.

## 5. Mutual exclusion falls out of D1

Single thread (D1 fibers): the framework and the firmware **never run
concurrently.** The framework touches firmware memory only when the firmware has
yielded to quiescence (it's swapped out); the firmware runs only between an
`advance` and the next quiescence. So firmware-memory access is never concurrent
— **no locking, no races** on firmware statics, for free.

## 6. C→Rust upcalls (framework callbacks)

The boundary is mostly Rust→C (the control ABI, §3) plus direct memory access
(§4). One direction goes the other way: **C→Rust upcalls**, used by the
simulated-interrupt model (D8, [`sim-interrupts.md`](sim-interrupts.md)) so the
sim HW-layer drivers can register interrupts with the framework
(`SIL_irq_registerOneShot(handler, delay_us, priority)`, etc.).

- **Mechanism:** at init, Rust passes C a struct of `extern "C"` function
  pointers; the sim drivers call through it. (No global function-pointer
  hunting; the vtable is handed in once.)
- **Control, not data** — scheduling/registration, not value transport — so it
  does not reintroduce the getter/setter pattern §4 removes.
- **Caller discipline:** invoked **only** from sim-target HW-layer code
  (`hw/<X>/sim/` + sim port glue), never from portable app/io/dev firmware, so
  the "firmware is sim-unaware" property holds.
- **Concurrency-safe for free:** single thread (§5) — an upcall is a plain
  synchronous call from the firmware fiber into framework code while the
  framework's main context is swapped out; nothing runs concurrently.

## 7. Safety & calling convention

- `extern "C"`, `#[repr(C)]` **only** where a struct is genuinely shared;
  prefer scalars / opaque handles across the boundary.
- All `unsafe` FFI quarantined in one `*-sys` crate; a safe wrapper sits above.
- Panics never cross FFI. Errors as return codes / out-params — matches the
  firmware's `bool HW_*_init` contract.
- The DWARF type map lets the safe layer reject a read/write whose Rust type
  doesn't match the entry's C type.

## 8. Build integration

- CMake gains a native **SHARED** firmware target (`-fPIC`, default-hidden
  visibility with the six `sil_fw_*` ABI functions exported). Globals need no
  export — DWARF/symtab carries them regardless.
- Rust locates the built artifact by path (decoupled builds in dev; CI chains
  `tools/build_native.sh` → cargo). A `build.rs` can optionally drive/locate
  the firmware build.
- Binding generation: the six ABI functions are hand-declared (trivial). No
  data API to bind — data is all DWARF-driven.

## 9. Portability

- **Windows:** PE + DWARF (MinGW gcc). `LoadLibrary` / `GetProcAddress`;
  `object`/`gimli` parse PE+DWARF.
- **macOS:** Mach-O; debug info via the `.dSYM` bundle / debug map. `dlopen` /
  `dlsym`. The dSYM lookup + debug-map handling is the fiddly part to validate
  on a spike.

## 10. Open implementation choices (not blocking)

- Exact crate layout: `sil-sys` (raw FFI + DWARF), `sil-core` (State/Route
  tables, engine, safe wrapper).
- Whether the fiber/context-switch primitives live entirely in the C port
  (likely) or are partly Rust-owned.
- Caching strategy for the DWARF symbol map (parse once at load, memoize).
