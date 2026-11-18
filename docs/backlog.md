# Firmware backlog

Deferred firmware tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. SIL-specific items live in `sil/backlog.md`.

## 20 kHz interrupt hot path

**When:** next — `sys~mc_006` fails on the bench and the trace loses
samples under drive. The target is under 20 us for the whole ADC interrupt
under drive.

**What remains:** at the requirement's condition (18 rad/s, four one-cycle
and 28 10 ms watches, 60 s; `docs/bench/cycle-timing.md`) the step maximum
is 21 us against 20, the callback maximum 42 us against 40, and the whole
interrupt reaches 50 us, the PWM period, averaging 32 us. The interrupt
takes 64 % of the CPU and the idle task under 2 %; the server task falls
to about 200 passes a second, the trace buffer overflows about a fifth of
the time and the app reports lost points. The USB transmit queue is not
full: the limit is CPU, not the link. With a 19-signal trace under
position control the interrupt takes 29.5 us typical / 37 us worst. The
largest costs by PC-sample profile are injected sample recording
(`Bridge::recordInjectedSample`), the CORDIC sine/cosine,
`lib_transforms_wrapAngle`, the cogging model's two CORDIC calls per cycle,
and trace sampling; the trace's send path (COBS encoding, the buffer copy
and capture, CRC) is about 13 % of samples at the four-watch load. Outside
the interrupt, the 1 ms task's encoder read is a blocking SPI transfer:
encoder RX over DMA is not wired.

**Done so far:** link-time optimization of the project's own code (idle
interrupt 21.1 -> 18.9 us; not yet measured under drive) and a count of
missed PWM cycles (`HW_ADC_data.irqMissedCycles`, `fw~hal_adc_012`).

**Candidates, from the compiled path:** `lib_transforms_wrapAngle` ahead of
`HW_CORDIC_sinCos`, which wraps again; the offset tracker's three passes a
cycle while driving; the divisions in Clarke, the modulator and the FOC
loops (reciprocals, or held from the 1 ms pass); the bus-voltage re-read in
`setVoltageVector`; `HAL_ADCEx_InjectedGetValue`'s rank switch; the
CORDIC's 24 iterations.

**How to measure:** `tools/bench/cycle_timing.py` for the requirement's
bounds; the probes `HW_ADC_data.irqDuration_us` and `main_cycleProbe_*`
read over SWD, plus DWT PC sampling, for a profile.

**Where:** `sw/lib/c/shared/io/bridge/`, `sw/lib/c/shared/app/motorControl/`,
`sw/lib/c/shared/lib/transforms/`, `sw/lib/c/shared/app/server/` (trace
sampling), `sw/lib/c/shared/hw/SPI/stm32g4/HW_SPI.c` (DMA path is TX-only).

## USB link: double-buffered bulk IN

**When:** when the trace needs more than ~500 kB/s sustained — e.g. more
than four one-cycle watches, or a higher PWM rate.

**What it is:** the link runs at the host's polling cadence, one 64-byte
packet every ~80 µs (~800 kB/s asymptote; 515 kB/s measured with
`tools/pcs_client.py --link-test`): the STM32 fsdev bulk IN endpoint is
single-buffered in TinyUSB 0.20, so every packet needs the completion
interrupt to reload it before the host's retry. The peripheral supports
double-buffered bulk endpoints (`dcd_stm32_fsdev.c` enables it for
isochronous only) and the PMA has room for a second 64-byte IN buffer.

**Link budget vs the physical path:** the trace admits a watch list against
a fixed 480 kB/s budget, but the link carries 241 kB/s through an external
VIA VL817 hub (four full-speed packets per ms through its transaction
translator) and 603 kB/s on a root port. Behind the hub a five-watch 20 kHz
list is admitted and overflows the ring; nothing on the device can tell the
two apart. Either measure the link at connect (the link test exists) and
admit against the measurement, or document "root port only" for 20 kHz work.

**Vendored patch carried:** `class/cdc/cdc_device.c` sizes the RX stream's
OUT transfer at `BULK_PACKET_SIZE` instead of `CFG_TUD_CDC_EP_BUFSIZE`, so a
host frame that is an exact multiple of 64 B completes without waiting for
the next frame. Re-apply on a TinyUSB upgrade. The SIL's USB model has no
packetization, so only the bench sees this; see "Sim USB: packetize the
host-to-device stream" in `docs/sil/backlog.md`. After the IN change,
re-measure with the link test and raise `APP_SERVER_LINK_BUDGET_BYTES_PER_S`
and its mirrors (Unity, SIL `trace_stream.rs`, GUI devmock and fallbacks).

**Where:** `sw/lib/c/tinyusb/portable/st/stm32_fsdev/dcd_stm32_fsdev.c`
(vendored; carry the patch or upstream it), `tusb_config.h`.

## Desktop app: trace stream robustness

**When:** before the trace client is relied on for long unattended
captures, or when a watch-list swap ever shows a stray sample.

**What it is:** four open items on the app's stream path.
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

**Backlog growth when the webview lags:** with five 20 kHz watches streaming
and the link unable to carry them, the renderer runs at over a core and its
working set grows ~57 MB/s, to gigabytes within minutes; the core's shedding
path (`host_dropped_points`) does not keep the backlog bounded. Find where
the points or batch events accumulate and bound it.

**Where:** `sw/gui/src-tauri/src/{trace.rs,emitter.rs,session.rs,protocol.rs}`,
`sw/lib/c/shared/proto/trace.proto` for (1), `app~conn_003`/`app~obs_003`.

## Trace stream: time-based telemetry cadence

**When:** if a watch list the budget admits ever runs the server task
below its 1 kHz pass rate.

**What it is:** telemetry is published every 100th server pass, so a pass
rate that sags under load takes the 10 Hz telemetry down with it. Every list
up to four fast channels holds 1 kHz passes on the bench today. A `nowMs`
hook in the server config would pin the cadence to time regardless.

**Where:** `app_server_run1ms` telemetry divider, `app_server_config_S`.

## Trace: burst capture and on-board envelopes

**When:** after the 20 kHz streaming trace has been used on the bench for
a while and one of these two limits actually bites.

**What it is:** two extensions the streaming trace leaves out. (1) *Burst
capture*: a scope-style mode where the cycle callback fills a fixed-layout
RAM buffer with many spans (more than the 4 one-cycle watches the stream
admits) for a short window, with a manual or threshold trigger and
pre-trigger history, frozen on completion and drained through the existing
128-byte memory read. Borrow the trace ring's arena; periodic tracing pauses
while a burst is armed. (2) *Envelopes*: a watch flag that keeps min and max
over the 20 cycles of a millisecond and ships both in the 1 ms record, so a
1 ms signal carries its sub-millisecond ripple at 1 ms bandwidth; the GUI
already draws min/max envelopes.

**Where:** `sw/lib/c/shared/app/server/app_server_trace.c` (capture),
`sw/lib/c/shared/proto/trace.proto` (a burst request and status), the
GUI trace client and history for the envelope record shape.

## SVM: sector-based alternative against the min/max form

`lib_transforms_svm` (`sw/lib/c/shared/lib/transforms/lib_transforms.h`) is
the min/max zero-sequence form; `fw~mc_014` pins the duty triple, not the
algorithm. An open study: implement the classic sector-based modulator
behind the same signature against the same reference test, and compare code
size, worst-case time in the 20 kHz step, and test complexity.

## HW_I2C stuck-bus recovery

**When:** when a bus that dies mid-operation must recover without a reboot —
at the latest, before motor operation depends on live CYPD3177 or gate-driver
status.

**What it is:** the HW_I2C driver handles per-transfer failure (NACK, error
IRQ, timeout + abort) but has no recovery for a bus whose wires are stuck: a
slave holding SDA low mid-bit, or SCL loaded so transfers can't clock (C28 on
I2C1 does this until its rework). In that state `HAL_I2C_Master_Abort_IT`
can't complete (the abort itself needs a working SCL), the HAL handle latches
in ABORT/BUSY, and every later transfer on that bus returns `HAL_BUSY`
immediately — the bus is dead until reset.

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
know; either extend `HW_I2C_busConfig_S` with the SCL/SDA pin identities or
route the recovery through HW_GPIO. Sim side: model a stuck bus (`sim/HW_I2C.c`
stall is per-transfer, not sticky-with-recovery) so the recovery path is
testable. Spec: extend `specs/firmware/hal/i2c.md` (likely a new
`fw~hal_i2c_005`) before implementing.

## App-initiated DFU firmware update over USB

**When:** when the desktop app takes on firmware updates. No firmware
groundwork blocks on it — the protocol seams it needs exist.

**What it is:** update the firmware from the desktop app with no debugger
attached. The G431's ROM system bootloader (AN2606) already speaks standard
USB DFU (DfuSe, AN3156) and enumerates as its own device (`0483:DF11`), so
the image bytes never travel over our protocol: the app's framed protocol
only *triggers* the transition and *verifies* the result. No resident custom
bootloader — the ROM is flash-free and unbrickable, and the 128 KB
single-bank part can't do A/B anyway.

**The design, when picked up:**

1. Schema: `EnterBootloaderRequest` in shared.proto's framework range
   (an action — answered with `Response` per the reply convention).
2. Server core: on accept, arm a ~100 ms countdown so the `Response`
   drains out the USB FIFO before the reset, then invoke a new board
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
   matches the flashed image (the identity service is the verification
   step; the DWARF gate re-arms automatically).
5. Specs first: a `sys~` update spec (`Needs: fw, app, test`), fw~ specs
   for the service + reset path, app~ specs for the orchestration state
   machine.

**Known warts:** Windows binds no driver to `0483:DF11` — first use needs
WinUSB via Zadig or the CubeProgrammer driver; the app should detect and
walk the user through it (macOS needs nothing). Interrupted flash: the G4
empty-check reboots into the ROM bootloader when bank start is erased, so
retry usually just works; confirm BOOT0 is physically reachable on the
board as the unconditional escape hatch before shipping the feature.

## IO_voltageMonitor: telemetry bus and rail scaling

**When:** before another consumer needs bus or rail values in engineering
units.

**What remains:** `IO_bridge`'s phase and bus current scaling is
config-driven; the telemetry hook in `sw/fw/src/app/server/app_server_config.c`
still reads raw pin volts (`HW_ADC_getVolts`) and scales bus voltage and bus
current inline (`APP_SERVER_VBUS_*`, marked by a TODO). An io-layer module
mapping logical channels (bus voltage, bus current, the rails) to
`(ADC channel, input, offset, scale, unit)` config would own that scaling.

**Where:** `sw/lib/c/shared/io/voltageMonitor/` + `sw/fw/src/io/` channel
config; spec under `specs/firmware/io/`.

## Sim USB: model the unconfigured (pre-enumeration) state

**When:** with the SIL transport work, or the next time USB lifecycle
behavior matters in a test.

**What it is:** the sim `HW_USB` has no notion of the device being
unconfigured: reads/writes touch its buffers from tick 0. Real TinyUSB is
unsafe there — before the host configures the device the CDC endpoints are
address 0, so a read during enumeration queues transfers on the control
endpoint and trips `TU_ASSERT(ep_status.busy == 0)` in `usbd_edpt_xfer`
(on the bench: "Device Descriptor Request Failed"). No native test can see
this class of bug.

**The fix, when picked up:** give the sim a `configured` flag distinct from
`connected` (mounted vs port-open, mirroring `tud_ready()` vs
`tud_cdc_connected()`): FIFO paths return 0 / accept nothing while
unconfigured, a DWARF-written data field drives it, and reset starts
unconfigured. Then a test asserting "no FIFO traffic before configuration"
turns this bench bug into a red test. Keep the guard in `stm32g4/HW_USB.c`
regardless.

## Own USB identity (VID/PID)

**When:** required before any public/field use (the placeholder VID squats
on TinyUSB's test space).

**What remains:** the board enumerates as `cafe:4001`
(`sw/lib/c/shared/hw/USB/stm32g4/HW_USB_descriptors.c`, with a TODO). The app
already shows `[vid:pid]` in its port list (`sw/gui/src-tauri/src/session.rs`).
Remaining: a free PID under the pid.codes VID (0x1209) dropped into
`USB_VID`/`USB_PID`, then the app filtering or badging ports by it — the
discovery seam the multi-device goal (sys~arch_004) wants;
`tools/pcs_client.py` can reuse the mapping. Windows names the port from the
usbser INF, not iProduct, so the identity, not a string, is what tells ports
apart.

## Low-side shunt window at the voltage ceiling

At 11.5 V on the 20 V bus the max SVM duty is ≈ 0.998, leaving ≈ 0.05 µs of
low-side on-time around the crest sample; the injected sample (trigger 0.5 µs
before the crest, last S/H ≈ 1.26 µs after) plus 0.3 µs dead time needs duty
≲ 0.93 (≈ 9.9 V). Order: scope the INA240 output at rated speed, then choose
between a duty cap and reconstructing from the two lowest-duty phases (W is
wired to ADC1 IN8), then gate the SIL sense model to match ("Current sense:
gate the sense model on the low-side on-state", `docs/sil/backlog.md`).

## ADC injected ISR: overrun loses a cycle silently

JEOC/JEOS are cleared after the control-step callback, so a callback that
overruns 50 µs swallows the next trigger's flags and that cycle is lost with
no count or fault.

## Break input under six-step: bench check

A real break (nFAULT / VDS trip) while six-step commutates should leave the
bridge dark and latch a fault (`fw~safety_003`). The path that restores a
clear master output enable after a channel start (`fw~hal_tim_008`) lives
only in the stm32g4 timer driver (`HW_TIM_setOutputEnabled`,
`sw/lib/c/shared/hw/TIM/stm32g4/HW_TIM.c`), which SIL and Unity do not
exercise. Trip a break on the bench mid-commutation and confirm.

## Capture export in the live Tauri app

The core command that opens the save dialog and writes the CSV
(`save_capture_csv`, `sw/gui/src-tauri/src/main.rs`) has run only through
the devmock. Exercise it once in the live app on Windows and macOS.

## App: named-sequence library has no delete

The Sequence widget saves sequences by name but offers no way to remove one.

## cubemx_to_channels.py does not emit `updateAtPeriod`

`HW_TIM_channels.cubemx.h` carries `updateAtPeriod` by hand; regenerating the
header with `tools/cubemx_to_channels.py` drops it. Teach the generator the
field (the sim-only `preload` flag lives in `HW_TIM_channels.c` and is safe).

## Bus overcurrent trip: one regular-ADC pass of lag

In SIL the bus-current trip latches 1100 to 2000 µs after the current
crosses 1.5 A, against 100 to 1000 µs for a phase trip: the bus current
comes from the regular ADC pass, so the 1 ms task reads the previous
pass's sample. `sys~safety_001`'s 2 ms bound holds with no margin;
`fw~safety_001`'s "within one control cycle" does not hold for the bus
current. Check on the bench whether the DMA pass completes before the
1 ms task reads it, then either reorder the read or restate the bound
(`sw/sil/pcs_bldc_sil/tests/faults.rs` measures it).

## dev_CYPD3177 reads TYPE_C_STATUS four bytes wide

`dev_CYPD3177.c` reads four bytes at 0x100C, which overlaps `BUS_VOLTAGE`
at 0x100D, so the exposed `typeCStatus` carries the bus-voltage byte in
bits 15:8. Read the register at its own width and check what consumes
the word.

## UI suite: the views_017 plot-probe check is timing-dependent

"views_017 probe found an on-trace spot inside the release strip" fails
occasionally when the machine is loaded and passes on the next run
(`sw/gui/tests/test_views.py`). Make the probe wait on the rendered trace
rather than on time.
