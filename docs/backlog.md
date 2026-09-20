# Firmware backlog

Deferred firmware tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. SIL-specific items live in `sil/backlog.md`.

## USB link: double-buffered bulk IN

**When:** when the trace needs more than ~500 kB/s sustained — e.g. more
than four one-cycle watches, or a higher PWM rate.

**What it is:** measured 2026-09-15 with `tools/pcs_client.py --link-test`
(256 B frames, zero drops): 205 kB/s at the start of the day, CPU-bound in
the server task on a bitwise CRC-32; 515 kB/s after the table CRC, a
512-byte CDC endpoint transfer, and `-O2` on the link path (the list in
`sw/lib/c/CMakeLists.txt`). Per byte the link now runs at the host's
polling cadence, one 64-byte packet every ~80 µs (~800 kB/s asymptote):
the STM32 fsdev bulk IN endpoint is single-buffered in TinyUSB 0.20, so
every packet needs the completion interrupt to reload it before the
host's retry. The peripheral supports double-buffered bulk endpoints
(`dcd_stm32_fsdev.c` enables it for isochronous only) and the PMA has
room for a second 64-byte IN buffer.

**Where:** `sw/lib/c/tinyusb/portable/st/stm32_fsdev/dcd_stm32_fsdev.c`
(vendored; carry the patch or upstream it), `tusb_config.h`. Re-measure
with the link test; then raise `APP_SERVER_LINK_BUDGET_BYTES_PER_S` and
its mirrors (Unity, SIL `trace_stream.rs`, GUI devmock and fallbacks).

## Desktop app: trace stream robustness (review 2026-09-19)

**When:** before the trace client is relied on for long unattended
captures, or when a watch-list swap ever shows a stray sample.

**What it is:** four items a review of the app's stream path left open.
(1) A watch-list install swaps the host demux table only after the device
accepts, so a message of the other list decodes onto the wrong signals
when the two lists' per-period record sizes match; a list generation
echoed from `WatchRequest` into `Samples` would let the host drop
mismatched messages. (2) The UI emitter's bounded queue sheds only when
`app.emit` blocks, and a non-tracing tauri build posts to the event loop
without blocking, so under a sustained backlog the points pile up in the
event-loop proxy and the JS heap where nothing is counted — check
`host_dropped_points` ever moves on the bench and, if not, bound where
backpressure is visible (an in-flight counter decremented from JS).
(3) macOS gets neither the 1 MB receive queue (`SetupComm` is Windows)
nor any host-side loss count: deframe/decode failures are silent and
surface as device drops. (4) An in-flight request keeps the duplicated
port handle open for up to its 500 ms timeout after disconnect,
delaying the DTR drop the board clears its list on.

**Where:** `sw/gui/src-tauri/src/{trace.rs,emitter.rs,session.rs,protocol.rs}`,
`sw/lib/c/shared/proto/trace.proto` for (1), `app~conn_003`/`app~obs_003`.

## Trace stream: time-based telemetry cadence

**When:** if a watch list the budget admits ever runs the server task
below its 1 kHz pass rate again.

**What it is:** telemetry is published every 100th server pass, so a
pass rate that sags under load takes the 10 Hz telemetry down with it
(seen 2026-09-19 before the pass-cost fixes: 1-2 Hz at two fast + one
1 ms watch). Since then a pass costs one USB transfer
(`fw~conn_server_006`) and the Samples path encodes the payload alone
(`lib_protobuf_encodeEnvelope`), and every list up to four fast channels
holds 1 kHz passes with 10 Hz telemetry on the bench. A `nowMs` hook in
the server config would pin the cadence to time regardless.

**Where:** `app_server_run1ms` telemetry divider, `app_server_config_S`.

## Trace: burst capture and on-board envelopes

**When:** after the 20 kHz streaming trace has been used on the bench for
a while and one of these two limits actually bites.

**What it is:** two extensions the fast-trace design deliberately left
out. (1) *Burst capture*: a scope-style mode where the cycle callback
fills a fixed-layout RAM buffer with many spans (more than the 4 one-cycle
watches the stream admits) for a short window, with a manual or
threshold trigger and pre-trigger history, frozen on completion and
drained through the existing 128-byte memory read. Borrow the trace
ring's arena; periodic tracing pauses while a burst is armed. (2)
*Envelopes*: a watch flag that keeps min and max over the 20 cycles of a
millisecond and ships both in the 1 ms record, so a 1 ms signal carries
its sub-millisecond ripple at 1 ms bandwidth; the GUI already draws
min/max envelopes.

**Where:** `sw/lib/c/shared/app/server/app_server_trace.c` (capture),
`sw/lib/c/shared/proto/trace.proto` (a burst request and status), the
GUI trace client and history for the envelope record shape.

**Origin:** fast-trace design discussion (2026-09-14); both judged
unnecessary for the first 20 kHz slice.

## SVM formulation comparison

**When:** after the V/f branch lands with the sector-based modulator
(`fw~mc_014~1`) and its reference test.

**What it is:** `fw~mc_014~1` pins the duty triple, not the algorithm, so a
min/max zero-sequence-injection modulator (and any other formulation with
symmetric zero-vector placement) satisfies the same spec and the same
reference test. Implement the alternative behind the same signature and
compare on code size, worst-case execution time in the 20 kHz step, and
test complexity; keep whichever wins, or keep both behind a build option
if the difference is instructive.

**Where:** the modulator module under `sw/lib/c/shared/lib/` and its
Unity reference test.

**Origin:** V/f + SVM spec interview (2026-09-14) — owner curiosity about
whether the classic sector-based form earns its extra code.

## HW_I2C stuck-bus recovery

**When:** when a bus that dies mid-operation must recover without a reboot —
at the latest, before motor operation depends on live CYPD3177 or gate-driver
status. No urgency while the buses either work (I2C3) or are down for
hardware reasons no software can fix (I2C1 until the C28 rework).

**What it is:** the HW_I2C driver handles per-transfer failure (NACK, error
IRQ, timeout + abort) but has no recovery for a bus whose wires are stuck: a
slave holding SDA low mid-bit, or SCL crippled so transfers can't clock. In
that state `HAL_I2C_Master_Abort_IT` can't complete (the abort itself needs a
working SCL), the HAL handle latches in ABORT/BUSY, and every subsequent
transfer on that bus returns `HAL_BUSY` immediately — the bus is dead until
reset.

**Observed on the bench (2026-07-05):** with C28 loading I2C1's SCL, the
first CYPD3177 fetch after boot burns its timeout + abort drain, the handle
wedges, and all later fetches fail instantly (`task200ms_us` ~300 µs steady
state instead of 7 × ~2 ms timeouts). Harmless today — the bus is physically
unusable anyway and the state clears on reset — but it demonstrates the
failure mode end to end.

**The fix, when picked up:**

1. Detect the wedge in `HW_I2C_private_transfer`: HAL start call returns
   `HAL_BUSY` while the driver believes no transfer is in flight (or N
   consecutive instant failures on a bus).
2. Standard 9-clock recovery: reconfigure SCL as a GPIO, clock up to 9 pulses
   until the slave releases SDA, generate a manual STOP (SDA low→high while
   SCL high).
3. `HAL_I2C_DeInit` / `HAL_I2C_Init` (+ analog/digital filter reconfig, as in
   `HW_I2C_init`) to clear the latched HAL/peripheral state, then restore the
   pins to AF open-drain.
4. Rate-limit recovery attempts (e.g. once per second) so a hard-failed bus
   doesn't turn the 200 ms task into a GPIO-thrash loop.

**Where:** `sw/lib/c/shared/hw/I2C/stm32g4/HW_I2C.c` (detection + reinit) —
the pin bit-bang needs the bus's GPIO port/pin, which the driver doesn't
currently know; either extend `HW_I2C_busConfig_S` with the SCL/SDA pin
identities or route the recovery through HW_GPIO. Sim side: model a stuck bus
(`sim/HW_I2C.c` stall is per-transfer today, not sticky-with-recovery) so the
recovery path is unit-testable. Spec: extend `specs/firmware/hal/i2c.md`
(likely a new `fw~hal_i2c_005`) before implementing.

**Origin:** cypd3177-i2c branch review finding (2026-07-04), confirmed by the
C28 bench behavior above.

## App-initiated DFU firmware update over USB

**When:** after the basic desktop app is up (it owns the orchestration and
the UX). No firmware groundwork blocks on it — the protocol seams it needs
already exist.

**What it is:** update the firmware from the desktop app with no debugger
attached. The G431's ROM system bootloader (AN2606) already speaks standard
USB DFU (DfuSe, AN3156) and enumerates as its own device (`0483:DF11`), so
the image bytes never travel over our protocol: the app's framed protocol
only *triggers* the transition and *verifies* the result. No resident custom
bootloader — the ROM is flash-free and unbrickable, and the 128 KB
single-bank part can't do A/B anyway.

**The design, when picked up:**

1. Schema: `EnterBootloaderRequest` in shared.proto's framework range
   (field 7, an action — answered with `Response` per the reply
   convention).
2. Server core: on accept, arm a ~100 ms countdown so the `Response`
   drains out the USB FIFO before the reset, then invoke a new third board
   hook `bool (*enterBootloader)(void)` in `app_server_config_S` (NULL →
   rejected "unsupported").
3. Board hook (the G4 mechanism): write a magic word to a `.noinit` RAM
   location, `NVIC_SystemReset()`; check the magic at the very top of
   `Reset_Handler` (before .data/.bss init and clocks), clear it, set MSP
   from `0x1FFF0000`, jump to system memory. Never jump to ROM from a
   running app with USB/PLLs live.
4. App orchestration: send request → close port → hotplug-watch CDC
   disappear + DFU `0483:DF11` appear → DfuSe erase/download (dfu-core
   crate or dfu-util/CubeProgrammer) → DFU "leave" → CDC re-enumerates →
   reconnect → `IdentityRequest` and assert the reported build identity
   matches the flashed image (the existing identity service is the
   verification step; the DWARF gate re-arms automatically).
5. Specs first: a `sys~` update spec (`Needs: fw, app, test`), fw~ specs
   for the service + reset path, app~ specs for the orchestration state
   machine.

**Known warts:** Windows binds no driver to `0483:DF11` — first use needs
WinUSB via Zadig or the CubeProgrammer driver; the app should detect and
walk the user through it (macOS needs nothing). Interrupted flash: the G4
empty-check reboots into the ROM bootloader when bank start is erased, so
retry usually just works; confirm BOOT0 is physically reachable on the
board as the unconditional escape hatch before shipping the feature.

**Origin:** serial-protocol branch design discussion (2026-08-15), as a
thought experiment validating the app_server board-hook extension pattern.

## IO_voltageMonitor: logical measurement channels over HW_ADC

**When:** before more consumers need engineering-unit measurements — the
signal-trace and control work will want the same bus/rail/phase values the
telemetry hook reads today.

**What it is:** analog measurements have no owning module. The
app_server telemetry hook reads raw pin volts (`HW_ADC_getVolts`) and
applies the sense-front-end scaling inline (`APP_SERVER_VBUS_*` defines),
and the retired Teleplot scaffolding did the same for phase
currents/voltages and rails. An io-layer `IO_voltageMonitor` would map
logical channels (VBUS_V, VBUS_I, RAIL_5V0, phase senses, ...) to
`(ADC channel, input, offset, scale, unit)` channel config, returning
engineering units (volts and non-volt units like amps) from one place.

**Where:** `sw/lib/c/shared/io/voltageMonitor/` + `sw/fw/src/io/` channel
config carrying the board's divider/shunt/gain constants; spec under
`specs/firmware/io/`. The app_server hook and IO_bridge's current-sense
scaling become consumers.

**Origin:** TODO in `app_server_config.c` (2026-08-16), serial-protocol
branch.

## Sim USB: model the unconfigured (pre-enumeration) state

**When:** with the SIL transport work, or the next time USB lifecycle
behavior matters in a test.

**What it is:** the sim `HW_USB` (loopback) has no notion of the device
being unconfigured: reads/writes touch its buffers from tick 0. Real
TinyUSB is unsafe there — its CDC FIFO paths claim endpoints unguarded,
and before the host configures the device the CDC endpoints are address
0, so a read during enumeration queues transfers on the control endpoint
and trips `TU_ASSERT(ep_status.busy == 0)` in `usbd_edpt_xfer`. This
exact failure shipped on the serial-protocol branch (server task pumping
`tud_cdc_read` every 1 ms from boot → "Device Descriptor Request Failed"
on the bench) and no native test could see it.

**The fix, when picked up:** give the sim a `configured` flag distinct
from `connected` (mounted vs port-open, mirroring `tud_ready()` vs
`tud_cdc_connected()`): FIFO paths return 0 / accept nothing while
unconfigured, a `HW_USB_sim_setConfigured()` hook drives it, and reset
starts unconfigured. Then a unit test asserting "no FIFO traffic before
configuration" turns this class of bench bug into a red test. Keep the
guard in `stm32g4/HW_USB.c` regardless — defense in depth at the layer
that owns the constraint.

**Origin:** bench enumeration failure on the serial-protocol branch
(2026-08-16), root-caused to the unguarded TinyUSB CDC read path.

## OFT trace scan time (build output under sw/)

**When:** when the ~9 s `oft trace` round trip starts to chafe (it runs
after every spec edit); it will grow with the app's dependency tree.

**What it is:** the canonical invocation `tools/oft/oft.sh trace specs/
sw/ README.md` crawls everything under `sw/`, which now includes Rust
build output (`sw/gui/src-tauri/target/` ~200 MB after the Tauri
scaffold, `sw/sil/target/`, `sw/lib/rust/*/target/`). OFT skips binary
content but still visits every file; measured 2026-08-18: ~9 s wall vs
~5 s before the app scaffold. No correctness impact — the 42-defect
baseline is unchanged, no false tags.

**The fix, when picked up:** either (a) have `oft.sh` (or a thin
wrapper) enumerate source roots instead of `sw/` wholesale — `sw/fw
sw/lib/c sw/proto sw/sil/voyant sw/sil/pcs_bldc_sil sw/lib/rust/*/src
sw/gui/src-tauri/src sw/gui/dist` — accepting the list needs upkeep as
the tree grows, or (b) stage a filtered copy/file-list excluding any
`target/`, `build/`, `gen/` dir before invoking OFT. Update CLAUDE.md's
verification command and CI in the same change so the scanned set stays
identical everywhere.

## Own USB identity (VID/PID + device name in the app's port list)

**When:** cosmetic until the port picker matters to someone other than
us; required before any public/field use (the placeholder VID squats on
TinyUSB's test space).

**What it is:** the board enumerates as TinyUSB's placeholder
`cafe:4001` (`sw/lib/c/shared/hw/USB/stm32g4/HW_USB_descriptors.c:7`,
which already carries the TODO). The descriptor strings are already
"pcs_bldc"/"pcs_bldc CDC", but Windows shows "USB Serial Device"
regardless — the in-box usbser driver's INF supplies the friendly
name, not the device's iProduct — so a string change alone won't fix
the port list.

**The fix, when picked up:**

1. Firmware: request a free PID under the pid.codes VID (0x1209) via
   their GitHub PR process; drop it into USB_VID/USB_PID. The new
   identity re-enumerates fresh (no stale friendly-name cache).
2. App: map the known VID:PID to the board's display name in
   `list_ports` (`sw/gui/src-tauri/src/session.rs`) — e.g.
   "pcs_bldc [1209:xxxx]" — and sort/badge matching ports first. This
   is also the discovery seam the multi-device stretch goal
   (sys~arch_004) wants: enumerate = filter by our VID:PID.
3. `tools/pcs_client.py` can reuse the same mapping for port hints.
