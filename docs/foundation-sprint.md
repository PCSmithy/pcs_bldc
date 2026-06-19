# Foundation sprint — spec back-fill, HW_DMA, full ADC

Pre-motor-control cleanup: get the existing driver foundation spec'd, traced,
and rounded out (DMA + all ADC channels) before starting the motor sprint.

## [START HERE] — next-session entry point

**Where we are:** Milestone 1 (driver spec back-fill) in progress. The **ADC
pilot is done** and establishes the repeatable workflow — `specs/firmware/hal/adc.md`
(7 specs, all `Covers: sys~arch_005~1`), `[impl->]` tags on both `HW_ADC`
drivers, a Unity suite (`sw/lib/c/shared/hw/ADC/sim/test/test_HW_ADC.c`), green
on ARM + native ctest, all 7 specs OFT-covered.

**Working tree is UNCOMMITTED** — the ADC pilot (spec + tags + sim multimode
model + tests + this doc) plus `docs/handoff.md` (staged). Commit it as the
first thing (suggested: one commit "Spec + test the HW_ADC driver (foundation
sprint pilot)").

**The repeatable recipe** (use the `pcs_spec` skill — it drives this):
1. Interview for intent (scope, what's in/out).
2. Fan out two `Explore` agents in parallel: a spec-tree cartographer (parent,
   placement, next ID, overlap/anti-bloat) and a codebase investigator (what
   the code actually does → testable acceptance + divergences).
3. Draft per `docs/spec-style.md`; then a **blind** style auditor subagent
   (only the draft + the two rule docs, no conversation context).
4. On approval: write the spec, add `[impl->]` tags (both target drivers) and a
   Unity test (both targets are tagged; tests run on the sim), then
   `validate-specs.py` + `oft.sh trace specs/ sw/`.

**Spec-first philosophy (important):** specs state the intended *final-state*
driver, not just what's built today. Specs may sit **OFT-uncovered** until the
impl lands — that is expected and fine; coverage climbs to 100% as we
implement. Do not water a spec down to match a current impl limitation; record
the limitation as a gap instead (see "Known impl gaps" below).

**Immediate next specs (milestone 1 remainder):**
- **AS5048 encoder** → `specs/firmware/est/encoder.md` (topic `est`). This is
  where we author the new **`sys~mc_001~1`** parent: "The system shall drive a
  BLDC motor using both sensored (encoder-based) and sensorless control
  algorithms." Add a `mc` row is unneeded (`mc` is already in the topic table).
- **SK6805** → `specs/firmware/obs/rgb_leds.md` (topic `obs`, parent `sys~arch_001~1`).
- **USB CDC** → `specs/firmware/conn/usb.md` (topic `conn`, parent `sys~arch_003~1`).
- **GPIO top-up** → extend `specs/firmware/hal/gpio.md` for the input cache
  (`HW_GPIO_run1ms`/`readCached`, added but untagged/unspec'd).

ADC checklist-pass cleanup (done): deleted the timeline-laden "Theory of
operation" block (folded the FOC motivation into a `Rationale:` on
`fw~hal_adc_006~1`); strengthened `fw~hal_adc_001~1` to require contiguous
regular ranks `1..N`; gave `fw~hal_adc_004~1` a pollable conversion-fault
status (`run1ms` stays `void`); and authored **`fw~hal_adc_008~1`** (async
conversion-completion model, parallel to `fw~hal_spi_005~1`) — OFT-uncovered
until the interrupt/DMA path is built.

Then milestones 2–5 below (app_rgbLedRing, HW_DMA, DMA integration, ADC channels).

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

Then: the **motor-control sprint**.

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
