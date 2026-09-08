# SIL bring-up — handover

Last updated: 2026-08-30. Orientation for picking up the SIL (software-in-the-
loop) effort in a fresh session. Read this first, then `README.md` +
`roadmap.md` in this folder. (`docs/handoff.md`, untracked, carries the
current session-to-session state.)

## TL;DR

**The injected-ADC / current-sense sprint is complete and up for merge
(sil -> main).** The sim now runs the timer-triggered injected ADC engine
against the fine-grid (50 µs) board world: one crest sample set per PWM
period at the CCR4 instant, injected U/V within 13.6 mA of the plant and
derived W within 16.8 mA (the 8 mA quantization floor), 800 consecutive
periods exact (`tests/crest_sampling.rs`). Members declare their own
**cadence** (`EveryStep`/`Periodic`/`OnInputChange`/`OnDemand` — encoders are
bus-driven: `DuplexPeer::transfer` carries a `MemberCtx` and samples the table
at the transaction instant), and after the perf pass the board world runs at
**4.8–5.3 µs/step, 9.4–10.4× realtime** (typical ~9.8×; `performance.md`
§16–§18).

The previous sprint's six-step commutation loop stands: button tap ->
alignment physically swings the rotor -> offset captured from the plant ->
dial demand -> closed-loop spin, driven and asserted purely through the State
Table (`tests/north_star.rs`).

The plant is bench-parameterized (iPower GM6208-150T, measured with the board
as the only instrument — see `tools/trace_analysis/*`): sinusoidal BEMF (it's
a PMSM), R_LL 29.35 Ω, Ke 0.55 V·s/rad, J 3.2e-4, B 7.2e-4, T_c 4.4e-3
(Coulomb/stiction with exact-zero park); `l_h` is the one class-typical
estimate left. With these params the sim reproduces the bench alignment snap
(~200 ms, parked, +2.2°e offset error).

Models (owner-implemented physics, never edit without asking): `motor.rs`
(hybrid ideal-diode legs + PMSM electrical + Coulomb mechanics), `as5048.rs`
(measured noise σ 1.52/1.57 LSB, seeded), `current_sense.rs` (mirrors
`IO_bridge_channels.c`). Port convention: models declare all ports in their
own namespace; `wiring.rs` (`wire_bridge` 7 delayed routes,
`wire_current_sense` 8 zero-latency routes) binds them; suspend-and-write on
a bundle is the fault-injection seam. Harness: the `pcs_bldc_sil` behavioral
suite plus voyant's unit tests, with per-test MF4 drops
(`PCS_SIL_TRACE_DIR=build/traces`).

Hardware findings this sprint (details in `backlog.md`): low-side-shunt
(1−duty) current visibility (six-step protection is safe; FOC needs the
reserved injected-ADC sampling); phase sequence U->W->V vs encoder-positive;
macOS keeps DWARF in dSYM bundles and clang -O3 SRA-decomposes small statics
into DW_OP_piece locations (both handled: `sil.rs` temp-copy carries the
bundle, `dwarf.rs` resolves pieces).

Bench capture tooling: `tools/serial_capture.py` (unbounded Teleplot->CSV
logger). The bench-only firmware duty-schedule/telemetry hooks were never
meant to ship; recover them from git history (`PCS_BENCH_DUTY_SEQ`) if a
future campaign (e.g. `l_h`) needs a starting point.

## Build & run

```bash
tools/run_sil.sh            # RELEASE (default): -O3 -flto -g firmware DLL + --release voyant, run the sanity suite
tools/run_sil.sh --debug    # DEBUG: -O0 -g DLL + debug voyant (dev/introspection; slower, source-faithful)
tools/run_sil.sh --clean    # wipe the native build dir first
```

**Default is optimized on both sides** — the suite's performance numbers are only
meaningful optimized. Release and debug use separate build dirs
(`build/native-fw-release` vs `build/native-fw`) and cargo target dirs, so they
coexist. The dev/test flow (`tools/build_native.sh`, Unity) stays `-O0 -g`; the
release DLL rides a `PCS_OPT_LEVEL` cache knob (`-O3`) plus a `PCS_LTO` switch
(`-flto -ffat-lto-objects` compile + `-flto` link) in `native.cmake`, driven by
`build_native.sh --opt -O3 --lto` (mirrors the ARM toolchain's opt knob). The
suite prints a phase-isolated **performance report** at the end (firmware tick /
full step / engine floor / derived); see the dated baseline in `performance.md`
§11 and the -O3+LTO measurement in §14.

The suite loads the firmware DLL, drives it over the control ABI, and runs
named PASS/FAIL checks (exit nonzero on any failure): boot; all four real
FreeRTOS tasks advancing; State Table historian/enum/ZOH; and an end-to-end
path — inject an AS5048 SPI frame via DWARF write, assert the exact angle
comes out in the telemetry text captured by the sim USB driver. All
injection/inspection is white-box DWARF access (never the deprecated `_sim_*`
C APIs — see `backlog.md`).

Rust unit tests run with the suite (`run_sil.sh` tests the whole workspace);
standalone: `cd sw/sil && cargo test -p voyant`.

**Rust toolchain gotcha:** it's `stable-x86_64-pc-windows-gnu` (matches MinGW),
installed at `~/.cargo/bin`. The Bash tool's shell does NOT source `~/.bashrc`,
so prefix cargo commands with `export PATH="$HOME/.cargo/bin:$PATH"`. (The
user's own terminals get it via `~/.bash_profile`.)

## Code map

```
sw/sil/
  voyant/                 THE FRAMEWORK (generic, no pcs_bldc code)
    src/signal.rs           SignalId (sig_type:source:name[:modifier]) + Value
    src/state_table.rs      StateTable: registry + per-signal change-log history
                            + current cache + per-signal epsilon
                            + retention. Pure data, no FFI. One-shot,
                            last-writer-wins writes (no override/pin).
    src/backend.rs          Firmware (public handle): control ABI + cvar
                            sample-resolver (start/shutdown/advance_time/
                            dispatch_isr + read_cvar/write_cvar; the only
                            unsafe/DWARF part). An internal pub(crate) Backend
                            trait is the execution/test-double seam
                            FirmwareMember drives. Auto-derives the ASLR anchor
                            from export∩DWARF (no hardcoded symbol). Also
                            FirmwareMember: a firmware instance wrapped as a
                            Member — AUTO-mirrors the whole traceable cvar
                            namespace (enumerated from DWARF at enable,
                            array-size exclusion policy + skip/register), and
                            brackets each step's dispatch with an in-sync flush
                            of fresh cvars and an out-sync sweep of the whole
                            mirror; the ONLY thing that touches fw memory
                            (routes never do). dwarf.rs carries leaf
                            enumeration; state_table.rs a dirty set +
                            record_mirror/take_dirty_indices.
    src/member.rs           Member trait (the one seam the engine drives everything
                            through: name/advance(dt,st)/set_enabled) + vsig_id +
                            RampModel reference model member. Members register their
                            own signals on the table (any time, any sig_type) and
                            push records each advance.
    src/route.rs            RouteTable: source->destination transport, one
                            snapshot-then-write pass/tick, add/remove/suspend/resume.
                            propagate is TABLE-ONLY (no backend): records src entry
                            -> dst entry; dst = any registered signal; both endpoints
                            checked at propagate; fault injection = suspend the route,
                            then write the destination directly.
    src/log.rs              Unified log: LogLevel/LogEntry + drop-oldest LogRing the
                            StateTable stamps with sim time (st.log/take_logs).
    src/dwarf.rs            DwarfMap: resolve var.member/arr[i] paths -> Leaf
                            (Scalar | Enum), incl. enum value->name
  pcs_bldc_sil/           THE INSTANTIATION (board-specific)
    src/main.rs             the perf report binary (phase-isolated + board rows)
    src/sil.rs              Sil harness: owns an Engine (Deref), loads firmware
    src/board.rs            the board world: fw + plant + encoders + sense, wired
    src/motor.rs            plant (owner physics: PMSM + ideal-diode legs + Coulomb)
    src/as5048.rs           AS5048 model (OnDemand duplex peer, measured noise)
    src/current_sense.rs    sense front end (OnInputChange affine chain)
    src/wiring.rs           route bundles (bridge, sense) = fault-injection seams
    tests/                  the behavioral suite (north_star, crest_sampling, ...)
  spike/d1-tick/          standalone D1 spike: cooperative fiber FreeRTOS port
                            determinism + throughput test (throwaway scaffolding)

sw/lib/c/FreeRTOS/portable/Native-Fiber/   the native cooperative fiber port
sw/lib/c/shared/hw/sim/ports/              SIL_ports: the null-safe C helper sim
                                            drivers use to register/read/write ports
sw/fw/src/sil_fw.h                          the control ABI (setHooks/setIrqHooks/start/
                                            advance_time/dispatch_isr/shutdown)
sw/fw/src/main.c                            SIM path: fiber-port bring-up + ABI impl
sw/fw/src/hw/sim/FreeRTOSConfig.h           native FreeRTOS config
tools/run_sil.sh                            one-command build + run

docs/sil/*.md            the design (see "Design docs" below)
```

## What's done

- **Design (D1–D12): all decided + captured** in `docs/sil/`. Key choices:
  single-threaded cooperative **fiber** execution (determinism + perf); native
  firmware as a **shared library** driven in-process; DWARF white-box; **State
  Table + Route Table** with the State Table *being* the historian; comms
  captured as state via sim-HW upcalls; generic **`voyant`** framework +
  `pcs_bldc_sil` instantiation.
- **Firmware on the SIM target:** FreeRTOS runs on native via the hand-written
  cooperative fiber port; the firmware builds as `libpcs_bldc_fw.dll` exporting
  the `sil_fw_*` control ABI; both native and ARM targets build green. The SIM
  target runs the SAME four FreeRTOS tasks (`task_1ms`, `task_10ms`, `task_usb`,
  `telemetryTask`) and the same io/dev/app init as embedded, via shared
  `prvHwInit`/`prvAppInit`/`prvCreateTasks` in `main.c`; target gating survives
  only at the hw-layer seam plus `HAL_Init`/timebase/`__io_putchar` and the
  per-target entry points. Embedded ELF impact of the whole SIL seam: +~80 B
  flash, +16 B bss.
- **`Member` is THE public framework seam** (`name`/`cadence`/`advance(dt,st)`/
  `set_enabled(on,st)`). Plant models and the firmware (via `FirmwareMember`)
  are both members; the engine holds a `Vec<Box<dyn Member>>` and advances in
  registration order, which is therefore an explicit design surface. `Backend`
  and `PortDef` are `pub(crate)` execution/test-double plumbing behind it.
- **The State Table is dumb data** — signals + per-signal change-logged history,
  one-shot **last-writer-wins** writes, per-signal epsilon (default 1e-3), O(1)
  current cache, O(log n) `value_at` ZOH, time-based retention (`None` =
  unbounded fast mode). A value persists exactly when nothing else writes it;
  persistence is the *absence of writers*, not a framework hold, so fault
  injection is **suspend the route, then write its destination directly**.
  History is **columnar** (per-signal typed scalar columns, kind fixed at first
  record; a boxed column for `Enum`/`Bytes`), so a later type mismatch is a bug
  and is rejected rather than migrated — see `signal-trace.md` §1.
- **Routes are a pure table operation** — `propagate` takes no `Backend`; it
  records source entries into destination entries and nothing else, with any
  registered signal of any `sig_type` legal as a destination. Per-route
  `latency` splits them: **delayed** (snapshot-then-write once at step start,
  from end-of-previous-step values) and **zero-latency** (fresh reads in cached
  topological order, re-run before each member, so a chain `a→b→c` resolves in
  one step). `RouteTable::validate` enforces single-driver, acyclic
  zero-latency graph, and forward-flow; the engine caches the verdict + topo
  order behind a wiring-dirty flag and re-raises a bad one at every `step`.
- **Ports are the driver-facing seam** — signals the sim HW drivers register at
  runtime in **native format** (volts stay volts; the driver owns conversion),
  through the `SIL_ports` null-safe C helper over the `sil_fw_setHooks` vtable.
  With no hooks installed the drivers behave exactly as standalone, so Unity
  runs are untouched.
- **The cvar mirror is automatic** — at enable a `FirmwareMember` enumerates
  every traceable DWARF leaf (recurse structs, expand arrays, array-size
  threshold to drop stacks/buffers, multi-dim/pointer skip) and registers
  `cvar:<member>:<leaf>`. Out-sync **sweeps** them memory→table via a
  shadow-snapshot `memcmp` over 64 B chunks (cost O(changed bytes), on a
  sim-time cadence); in-sync **flush is sparse**, drained from the table's dirty
  set by integer index. `skip_cvar_registration_by_prefix` /
  `register_cvar_in_state_table` tune the policy. The firmware member is the
  ONLY thing that touches firmware memory — routes never do.
- **Members declare their own cadence** (`EveryStep`/`Periodic`/`OnInputChange`/
  `OnDemand`), so cost tracks events rather than the grid — `member-cadence.md`.
- **Interrupts are a framework-owned table** (D8): periodic, one-shot, and
  pended entries, registered at config time by name or at runtime by pointer
  through the `sil_fw_setIrqHooks` upcall vtable; priority-then-registration
  ordering, per-entry enable, masked-holds-pending. Dispatch runs in the
  firmware fiber inside the port's ISR entry/exit bracket, so `...FromISR`
  wakeups and `portYIELD_FROM_ISR` behave as on hardware. The kernel tick is a
  plain table entry, as on silicon — `sim-interrupts.md`.
- **Unified log** (`log.rs`): `LogLevel`/`LogEntry` + a drop-oldest `LogRing`
  the State Table owns and stamps with sim time (`st.log`/`take_logs`).

- **Commutation sprint (merged as PR #4, 2026-08-10):** duplex SPI seam +
  AS5048/motor/current-sense models + `wiring.rs` bundles + the six-step
  closed-loop north star (`tests/north_star.rs`); plant bench-parameterized
  (see TL;DR ¶2–3).

- **Injected-ADC / current-sense sprint (2026-08-10 → 2026-08-30, this
  merge):** in stage order —
  - Sim TIM trigger seam carries crossing direction; TIM1 on TRGO2/OC4; the
    **timer-triggered injected ADC engine** (closes `fw~hal_adc_003/_008`):
    one TRGO sink fans out per channel, slots sample their pin's port at the
    trigger instant, completion is a **NVIC-style pended interrupt** drained
    in the firmware fiber. Per-entry dispatch counts
    (`isr_dispatch_count_of`); one world-total canary assert stays deliberate.
  - **Fine-grid board world north star** (50 µs grid): crest sampling
    U/V/derived-W against the plant, 800 periods exact, regular path intact
    beside it. Bench matrix A/B/C1–C3 + JEOS re-check all verified on
    hardware; ADC IRQ at priority 4 with an RTOS-free callback contract.
  - **Zero-latency delivery**: port `in_sync` runs before `advance_time`, so
    trigger-instant sampling reads the same step's routed values; the north
    star is retightened to same-step.
  - **Perf pass** (25.0 → 4.8–5.3 µs/step, 2.0× → **9.4–10.4× realtime**,
    typical ~9.8×): motor
    integrator sub-step 1 → 5 µs (owner constant, measured-identical);
    `SigHandle` resolve-once model IO; **member-declared cadence**
    (`member-cadence.md`: `Cadence` on the `Member` trait, encoders
    `OnDemand`/bus-driven with `MemberCtx` in `DuplexPeer::transfer` — a
    dispatch-window table stash carries it across the C frame — sense
    `OnInputChange` on post-epsilon input-dirty bits, fw port-fill gated,
    dirty-at-birth rule); duplex `:tx`/`:rx` interning + the index-keyed
    cvar flush drain (`take_dirty_indices`). Ledger: `performance.md`
    §16–§18. Engine `tick_period_us` → `grid_us`; `run_sil.sh` tests the
    whole workspace in release by default.

## What's next (prioritized)

> **Next sprint (likely): the FOC current loop.** The open design call ahead of
> it is whether `IO_bridge` owns the fast (injected) phase currents with
> counter-equalization pairing (owner leaning yes, not decided). The
> engine-side next-event queue (`sim-interrupts.md` §5) and the deferred
> cleanups remain parked in `backlog.md`.

1. **Comms** — the `Transport` trait + sim-HW upcall capture (logical payloads),
   routed to peer models (external transport / desktop app later, D5).
2. **Run modes** (fast/realtime pacing) + **Python bindings** (pytest, D3) +
   **dashboard** (D4). **Perf seams** from `performance.md` (zero-alloc hot loop,
   gated discrete work, dirty-page historian scan) — bake in as the loop grows.

## Open threads / pending decisions

- **Trait seams — `Transport` + `Scenario` remain.** The generic-framework
  vision (`architecture.md` §7) still wants `Transport` (comms) and `Scenario`
  (wiring), which land with their chunks (#1 / run-config). `Member` is the
  public seam everything else already goes through.
- **Coercion follow-ups (low priority, accepted):** `i64` firmware fields narrow
  to `Value::I32` (rare; user OK with it); unknown enumerators read as `<n>`.
- **Deferred cleanups live in `backlog.md`** — notably `HW_USB_sim.h`, the one
  surviving `HW_<Module>_sim.h` inject/inspect header (the protocol sprint
  rebuilt the sim USB on it), and the unit tests that lean on it.

## Design docs (source of truth)

In `docs/sil/`: `README.md` (index) · `architecture.md` (the picture + decisions
table) · `roadmap.md` (phase-by-phase status) · `state-route-tables.md`
(State/Route Table + naming + comms) · `signal-trace.md` (D12 historian) ·
`freertos-tick.md` (D1 fiber port) · `ffi-boundary.md` (D2) ·
`sim-interrupts.md` (D8) · `performance.md` (perf levers) · `inverter-timestep.md`
(D6) · `time-virtualization.md` (D9) · `determinism.md` (D7).
