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
hand-written C surface is a tiny **control ABI** (lifecycle + tick advance).
Fast-mode parallelism is process-level (`pytest -n`).

---

## 1. In-process, one instance per process

The firmware is a large bag of C global state — the FreeRTOS kernel, task
stacks, `static HW_ADC_data`, etc. Two independent firmware instances cannot
coexist in one OS process. That single constraint settles the model:

- **In-process:** Rust framework + plant models + firmware share one process.
  The D1 firmware-thread ↔ framework-thread handshake is in-process thread
  sync — cheap, no IPC.
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

Lifecycle + the D1 advance handshake. This is **control, not data** — you
cannot "advance the scheduler" by poking a variable, so these stay functions.
The firmware thread is created C-side, so Rust sees synchronous calls and the
threading stays hidden in the port layer.

```c
// sil_abi.h — the entire stable Rust<->C surface
void sil_fw_start(void);        // spawn fw thread: HW_*_init + create tasks + run scheduler
void sil_fw_advance_tick(void); // signal "advance one tick"; return when quiescent (D1)
void sil_fw_shutdown(void);
```

There are **no sim-specific data functions** beyond this. The firmware is
otherwise unaware it is being simulated.

## 4. DWARF introspection — the State Table substrate

All firmware data access is by **direct in-process memory access**, located
via debug info. This is what makes the State Table ([`state-route-tables.md`](state-route-tables.md))
able to hold an entry for *every* firmware static with zero firmware changes.

- Native lib is built with `-g` and `-O0` (already in `native.cmake`). **-O0
  matters**: every `static` (including function-local statics) keeps a real,
  stable address and isn't elided, so the whole set is enumerable and
  addressable.
- Parse the lib once with the `object` (ELF/Mach-O/PE) + `gimli` (DWARF)
  crates to build a map: `name → { link address, type }`. DWARF gives struct
  layout, enums, arrays — so nested members flatten to addressable leaves
  (`HW_ADC_data.channelData[0].counts[3]` = base + offset).
- In-process, a global lives in our own address space — reading/writing it is a
  pointer dereference, **no ptrace / no cross-process memory API**.
- Resolve ASLR once: `dlsym` a known **anchor symbol**, compare its runtime
  address to its link address → the load slide → apply to every entry's link
  address to get its live address.

Typed access (`read::<f32>(...)` / `write::<u32>(...)`) sits behind a safe Rust
wrapper that checks the requested Rust type against the entry's DWARF type.

## 5. Mutual exclusion falls out of D1

The framework touches firmware memory **only while the firmware thread is
parked at the idle-hook** (quiescent, between ticks). The firmware runs only
between an `advance` signal and the next quiescence. So firmware-memory access
is never concurrent — **no locking, no races** on firmware statics, for free.

## 6. Safety & calling convention

- `extern "C"`, `#[repr(C)]` **only** where a struct is genuinely shared;
  prefer scalars / opaque handles across the boundary.
- All `unsafe` FFI quarantined in one `*-sys` crate; a safe wrapper sits above.
- Panics never cross FFI. Errors as return codes / out-params — matches the
  firmware's `bool HW_*_init` contract.
- The DWARF type map lets the safe layer reject a read/write whose Rust type
  doesn't match the entry's C type.

## 7. Build integration

- CMake gains a native **SHARED** firmware target (`-fPIC`, default-hidden
  visibility with the ~3 `sil_*` ABI functions exported). Globals need no
  export — DWARF/symtab carries them regardless.
- Rust locates the built artifact by path (decoupled builds in dev; CI chains
  `tools/build_native.sh` → cargo). A `build.rs` can optionally drive/locate
  the firmware build.
- Binding generation: the ~3 ABI functions are hand-declared (trivial). No
  data API to bind — data is all DWARF-driven.

## 8. Portability

- **Windows:** PE + DWARF (MinGW gcc). `LoadLibrary` / `GetProcAddress`;
  `object`/`gimli` parse PE+DWARF.
- **macOS:** Mach-O; debug info via the `.dSYM` bundle / debug map. `dlopen` /
  `dlsym`. The dSYM lookup + debug-map handling is the fiddly part to validate
  on a spike.

## 9. Open implementation choices (not blocking)

- Exact crate layout: `sil-sys` (raw FFI + DWARF), `sil-core` (State/Route
  tables, engine, safe wrapper).
- Whether the firmware thread + handshake primitives live entirely in the C
  port (likely) or are partly Rust-owned.
- Caching strategy for the DWARF symbol map (parse once at load, memoize).
