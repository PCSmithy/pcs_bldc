# Foundation sprint — spec back-fill, HW_DMA, full ADC

Pre-motor-control cleanup: get the existing driver foundation spec'd, traced,
and rounded out (DMA + all ADC channels) before starting the motor sprint.

## Status: COMPLETE ✅

All five milestones (M1–M5) are done and bench-verified. The driver
foundation is spec'd, traced, dual-target-built, and the full board-sensing
path is verified in engineering units over CDC telemetry.

- **M1 — driver spec back-fill:** done (ADC pilot, then AS5048, SK6805, USB,
  GPIO top-up). 65 spec defs, `validate-specs.py` clean.
- **M2 — `app_rgbLedRing`:** done.
- **M3 — `HW_DMA`:** done.
- **M4 — DMA ↔ SPI ↔ SK6805:** SK6805 TX half done; encoder RX DMA half
  **parked** (low value, ~3.5 µs read — see `docs/handoff.md`).
- **M5 — remaining ADC channels + OPAMP:** done. All 10 board analog inputs
  wired and bench-verified; new `HW_OPAMP` driver (`fw~hal_opamp_001/002`)
  routes phase VSENSE via the internal op-amps.

OFT sits at the intentional 10-defect ahead-of-impl baseline (the `sys~`
anchors + `fw~hal_adc_003/008`, reserved for the motor sprint). Native ctest
green (14 suites); ARM links clean (~60 KB at `-Og`).

**Successor:** the next sprint's milestone doc is
[`docs/motor-sprint.md`](motor-sprint.md) — drive-path bring-up, the FOC
current loop, and velocity/position control + estimation. The rest of this
file is the historical record of how the foundation sprint was run.

## Current traceability baseline

- **GPIO, SPI — done.** `specs/firmware/hal/{gpio,spi}.md` exist with in-code
  `[impl->]`/`[test->]` tags and Unity tests. GPIO needs a small **top-up**:
  the input cache added recently (`HW_GPIO_run1ms` / `HW_GPIO_readCached`) is
  untagged and unspec'd.
- **ADC** — stub `fw~hal_adc_001~1` referenced only; no content, no tags.
- **AS5048, SK6805, USB** — no specs, no tags (scaffolding).

## Topic placement (decided)

| Driver | Spec file | Topic | Derives from (`sys~`) |
|---|---|---|---|
| ADC | `specs/firmware/hal/adc.md` | `hal` | `sys~mc_001` (current/voltage sensing) |
| AS5048 encoder | `specs/firmware/est/encoder.md` | `est` | `sys~mc_001` (sensored control needs rotor position) |
| SK6805 ring driver | `specs/firmware/obs/rgb_leds.md` | `obs` | `sys~arch_001` (standalone on-device status indication) |
| USB CDC | `specs/firmware/conn/usb.md` | `conn` | `sys~arch_003` (USB CDC transport) |
| GPIO input-cache top-up | extend `specs/firmware/hal/gpio.md` | `hal` | `sys~arch_001` / `sys~arch_005` |

New system anchor to add:
- **`sys~mc_001~1`** (topic `mc`) — "The system shall drive a BLDC motor using
  both sensored (encoder-based) and sensorless control algorithms." Anchors the
  encoder driver, ADC sensing, and the future motor-control work.
- LED indication is already covered by **`sys~arch_001~1`** — reuse, no new spec.

Note: `app~` = the desktop app. The firmware `app` layer (e.g. `app_rgbLedRing`)
is still `fw~`.

## Milestones (dependency order)

1. **Driver spec back-fill + linking.** Per driver: author spec → add
   `[impl->]`/`[test->]` tags → `tools/oft/oft.sh trace specs/` clean.
   Pilot on **ADC** to lock the workflow, then AS5048, SK6805, USB, GPIO top-up.
2. **`app_rgbLedRing` specs** (`fw~`, topic `obs`, derives `sys~arch_001`) for
   the LED-ring mode/UI behaviour currently scaffolded in `ledTask`. Impl
   drafted by the user afterward. Needs the `app/` layer wired into CMake
   (mirror the `dev/` setup).
3. **HW_DMA** — specs → impl → linking (new `hw` module, topic `hal`).
4. **HW_DMA ↔ HW_SPI ↔ SK6805 integration** — DMA-backed SPI transfers; SK6805
   uses them; remove the `vTaskSuspendAll` stream guard. Amend the SPI + SK6805
   specs. Depends on (3) and the (1) SPI/SK6805 specs.
5. **Remaining ADC channels** — instantiate all board channels + CDC printout
   for verification. Depends on the (1) ADC spec.

Then: the **motor-control sprint** — see [`docs/motor-sprint.md`](motor-sprint.md).

## Acceptance (every milestone)

- `tools/oft/oft.sh trace specs/ sw/` has no **dangling/broken** tags (a tag
  pointing at a missing/version-bumped ID). Uncovered specs are **expected and
  fine** — they're written ahead of the impl and drive it; coverage climbs to
  100% as code lands. Do not block on uncovered.
- `tools/validate-specs.py` clean.
- ARM + native build clean; native ctest green.

## Known impl gaps to close after specs are linked

The specs describe the intended final-state driver; the impl lags in places.
These are tracked gaps to close in the post-spec implementation pass, not spec
defects.

ADC (`HW_ADC`) — closed in the Phase 1/2 pass:
- ✅ **Regular-rank contiguity enforced** (`fw~hal_adc_001~1`). stm32g4 init
  now rejects gappy/duplicate regular ranks (must be contiguous `1..N`).
- ✅ **`run1ms` fault status** (`fw~hal_adc_004~1`). `run1ms` stays `void`; each
  channel exposes a pollable `HW_ADC_getStatus` (IDLE/OK/FAULT) set on a poll
  timeout. Sim models faults via `HW_ADC_sim_setConversionStall`; tested.
- ✅ **Wired into the runtime** (Phase 2). `HW_ADC_run1ms` runs in the 1 ms
  task; the USB task prints ADC1_IN6 / ADC2_IN11 counts + volts + status over
  CDC for hardware verification. (Hardware check still pending on-bench.)

ADC (`HW_ADC`) — still open (deferred to the motor-control sprint):
- **Init rejects unimplemented modes.** `HW_ADC_init` returns false for any
  trigger/transfer mode other than software+polled (an interim guard, now
  untagged). Per `fw~hal_adc_003~1` every valid mode must initialize; the guard
  falls away as the modes land.
- **Only software-triggered polled is implemented.** Timer trigger and
  interrupt/DMA transfer (`fw~hal_adc_003~1`, OFT-uncovered) are unbuilt —
  `run1ms` services only the polled path. Needed for FOC current sampling.
- **No async conversion-completion model.** The interrupt/DMA modes need a
  completion contract (callback + pollable status) analogous to
  `fw~hal_spi_005~1`. Spec'd by `fw~hal_adc_008~1` (OFT-uncovered); impl unbuilt.
- **Sim accepts `numBits` 1..31** vs the hardware's {6, 8, 10, 12} (low
  priority; spec is intentionally silent on resolution domain).
