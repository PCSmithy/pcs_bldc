# Spec system

This project uses spec-driven development with end-to-end traceability. The
core promise is simple:

- Every piece of application code links to a software requirement (`fw~` for
  firmware, `app~` for the desktop app).
- Every software requirement is both *implemented* (by code) and *verified*
  (by a test).
- Every system requirement is both *decomposed* (into one or more software
  requirements in the relevant component(s)) and *verified* (by a test).
- Every requirement derives from a parent — either a higher-level requirement
  or, at the top of the tree, a named section of the project goals in
  [`README.md`](../README.md).

The spec system is treated as a first-class deliverable, not as paperwork. Its
purpose is to make the project's intent legible to a future reader (or
future-me) and to make it impossible for code, specs, and tests to drift apart
silently.

## Tooling

The project uses **OpenFastTrace (OFT)** as the traceability tool, pinned at
version `4.2.2` and installed project-locally under
[`tools/oft/`](../tools/oft/). See that directory's `README.md` for setup
details. CI runs `oft trace` over the spec directories and source/test trees
and fails the build on any uncovered spec, broken link, or stale ID.

## Hierarchy

Deliberately shallow. The two software-component artifact types (`fw~` and
`app~`) sit side-by-side under `sys~`, and a `sys~` spec declares which
component(s) it requires for coverage on a per-spec basis:

```
Project goals  (README.md narrative, no OFT IDs)
    |
    |  derives
    v
sys~xxx   Needs declared per-spec, e.g.:
    |          - Needs: fw, test          (firmware-only behavior)
    |          - Needs: app, test         (desktop-app-only behavior)
    |          - Needs: fw, app, test     (cross-component behavior)
    |
    |  decomposed by  (one or more of fw~, app~)
    v
fw~xxx     Needs: impl, test              app~xxx    Needs: impl, test
    |              \---------\                |              \---------\
    |  implemented by         \               |  implemented by         \
    v                          v              v                          v
// [impl->fw~xxx~1]   // [test->fw~xxx~1]   // [impl->app~xxx~1]   // [test->app~xxx~1]
   in C/C++ source       in any test          in Rust source          in any test

System-level tests cover sys~ specs directly:
   // [test->sys~xxx~1]   in any test (typically SIL or end-to-end integration)
```

**No separate "low-level" software requirements layer.** If a `fw~` or `app~`
requirement is well-written and the implementing code is clear, that is
enough. Adding a mechanically-derived LLR layer below it is churn without
clarity. If a `fw~`/`app~` requirement turns out to need finer-grained
sub-requirements to be testable, those go in as additional `fw~`/`app~`
specs at the same level — not as a separate stratum.

## Coverage rule

We use five artifact types. The `Needs:` line on each spec tells OFT what
downstream coverage is required:

| Type   | Lives in                       | Covers (via OFT)         | Needs                                |
|--------|--------------------------------|--------------------------|--------------------------------------|
| `sys`  | `specs/system/...md`           | (parent: project goal)   | per-spec: some of `fw`, `app`, `test`|
| `fw`   | `specs/firmware/...md`         | a `sys~` parent          | `impl`, `test`                       |
| `app`  | `specs/desktop-app/...md`      | a `sys~` parent          | `impl`, `test`                       |
| `impl` | source code comments           | a `fw~` or `app~` spec   | (terminal)                           |
| `test` | test code comments             | any spec                 | (terminal)                           |

Concretely:

- A **`sys~` requirement is covered** when each component listed in its
  `Needs:` has at least one spec covering it *and* at least one `test`
  directly verifies the system-level behavior. System-level tests are
  typically SIL scenarios or end-to-end integration tests — SIL is the
  primary verification surface for this project.
- A **`fw~` or `app~` requirement is covered** when at least one piece of
  source code implements it *and* at least one test verifies it. The test
  can be a unit test, an integration test, or a SIL scenario — OFT does not
  distinguish, and we deliberately do not impose a per-layer rule. A
  comprehensive SIL test that happens to cover a `sys~` behavior *and*
  several supporting `fw~` specs is a perfectly valid coverage source for
  all of them.
- **Every `sys~` spec derives from a named section of `README.md`'s Project
  Goals.** OFT cannot enforce this link (the project goals do not carry OFT
  IDs), so it is enforced by review.
- **Every implementation tag points at a real `fw~` or `app~` spec.** New
  code without a spec to point at is a spec gap, not a license to skip the
  spec.

CI fails the build on any uncovered spec, any broken link, or any tag
pointing at a stale or version-bumped ID.

## File organization

Three top-level component folders under `specs/`, each subdivided by topic:

```
specs/
  README.md                           Top-level MOC; index of areas + conventions
  _template.md                        Working example of the spec format

  system/                             sys~ requirements
    README.md                         MOC for system-level specs
    motor-control/
      torque-control.md
      velocity-control.md
      position-control.md
      trajectory-tracking.md
    estimation/
    observability/
    operating-modes/
    safety/
    power-startup/

  firmware/                           fw~ requirements (STM32G4, C/C++)
    README.md                         MOC for firmware specs
    architecture/
    foc/
    estimation/
    motion/
    drivers/
    telemetry/
    safety/

  desktop-app/                        app~ requirements (Rust GUI)
    README.md                         MOC for desktop app specs
    architecture/
    connection/
    views/
    data/
```

Sub-folders are created when a topic gets its first spec. Do not pre-create
empty folders — that is a documentation graveyard waiting to happen.

The component folder (`system/`, `firmware/`, `desktop-app/`) and ID prefix
(`sys~`, `fw~`, `app~`) carry the same information; the redundancy is
deliberate so that both folder browsing and OFT reports stay legible.

## File granularity (one spec per file vs many)

- **One spec per file for `sys~` specs.** They are fewer, higher-level, and
  each deserves its own page with full context. The file's `# H1` title
  matches the spec's `### H3` heading (Obsidian-friendly).
- **Multiple specs per file for `fw~` / `app~` when they are tightly
  related.** For example, `firmware/foc/park-clarke-transforms.md` could
  hold both `fw~foc_park_transform~1` and `fw~foc_clarke_transform~1`
  because you would never read one without the other. Split into separate
  files when specs are independent.

Let granularity grow with content — start one-per-file and merge later if
you find yourself constantly opening pairs.

## Obsidian conventions

The `specs/` directory is designed to read well as an Obsidian vault as well
as plain Markdown on GitHub.

- **Frontmatter** on every spec file:
  ```yaml
  ---
  status: draft       # draft | accepted | superseded
  tags: [firmware, foc, mvp]
  ---
  ```
  `status` is a human signal (does not affect OFT). `tags` powers Obsidian
  queries and OFT's `--wanted-tags` filter.

- **`[[wiki-links]]`** in body text for narrative cross-references between
  specs. These are for human navigation; OFT ignores them. Formal trace
  always lives in `Covers:` and `Needs:` blocks.

- **MOC files** (`README.md` per folder) carry a brief narrative plus a
  list of `[[wiki-links]]` to the specs in that folder. They are the entry
  points for both GitHub readers and Obsidian users.

## ID conventions

OFT spec IDs follow the syntax `<type>~<topic>[_<subtopic>]_<NNN>~<version>`.
The body is intentionally short — the spec heading and content in the spec
file is the real description; the ID is just a stable index.

- **Type** marks the artifact type: `sys`, `fw`, or `app`.
- **Topic** is a short canonical abbreviation drawn from the table below.
- **Subtopic** is an *optional* second-level abbreviation used to give a
  busy topic per-area number spaces instead of one flat pool. It is
  currently used by the `hal` topic, which carries one sub-topic per
  peripheral: `hal_spi`, `hal_adc`, `hal_gpio`, `hal_dma`, `hal_tim`, ...
  Most topics omit the sub-topic entirely. Sub-topics are lowercase
  abbreviations; they are not separately enumerated in the topic table.
- **NNN** is a zero-padded 3-digit sequential number, scoped to the
  `(type, topic, subtopic)` tuple. `sys~mc_001`, `sys~mc_002`, ...,
  `sys~mc_999`. `fw~mc_001` is a separate number space from `sys~mc_001`,
  and `fw~hal_spi_001` is a separate number space from `fw~hal_adc_001`.
- **Version** is **always `~1`** by project policy. We do not use OFT's
  version-bumping mechanism; specs are edited in place. The trailing `~1`
  is mandatory OFT syntax we cannot elide. **Do not bump** unless you have
  a specific reason to invalidate downstream coverage and force re-tagging.

### Topic abbreviations

The canonical table. Adding a new topic? Add a row here first, then start
numbering at `_001`.

| Abbrev    | Topic                                            | Used by         |
|-----------|--------------------------------------------------|-----------------|
| `arch`    | Architecture (cross-cutting / overview)          | sys, fw, app    |
| `hal`     | Hardware abstraction layer (hw-layer peripheral drivers: SPI, ADC, GPIO, DMA, timers); uses per-peripheral sub-topics, e.g. `hal_spi`, `hal_adc` | sys, fw |
| `mc`      | Motor control (FOC, motion, trajectory tracking) | sys, fw         |
| `est`     | Estimation (Kalman, sensorless, parameter ID)    | sys, fw         |
| `obs`     | Observability (telemetry, logging, plotting)     | sys, fw, app    |
| `ops`     | Operating modes / mode state machine             | sys, fw         |
| `safety`  | Safety / fault handling                          | sys, fw         |
| `pd`      | Power delivery / startup                         | sys, fw         |
| `persist` | NVRAM / persistence                              | sys, fw         |
| `conn`    | Device discovery, connection, data handling      | sys, app        |
| `views`   | UI views (live plot, controls, config, diags)    | app             |

Sub-folders inside a topic folder (e.g. `firmware/mc/foc/` and
`firmware/mc/motion/` both under the `mc` abbrev) are fine and do **not**
require a separate abbrev — the spec ID still uses the parent topic's
short form.

### Numbering rules

- Sequential within a `(type, topic, subtopic)` tuple. To assign a new
  number, find the highest existing one in that same tuple and add 1.
- **Never reuse numbers.** If a spec is deleted or superseded, leave the
  gap. Numbers are stable identifiers; reused numbers cause silent trace
  breakage.

### Helper scripts

- **Allocating a new number** — use `tools/next-spec-id.py <type> <topic>`
  rather than grepping by hand:

  ```bash
  tools/next-spec-id.py sys mc          # -> sys~mc_001~1
  tools/next-spec-id.py fw mc           # -> fw~mc_007~1 (independent number space)
  tools/next-spec-id.py fw hal spi      # -> fw~hal_spi_001~1 (sub-topic space)
  tools/next-spec-id.py --list sys arch # list all existing + show next
  ```

- **Validating the whole tree** — `tools/validate-specs.py` walks `specs/`
  and checks every spec ID against the convention (valid type, topic in
  the canonical table, 3-digit zero-padded number, version `~1`, and
  uniqueness):

  ```bash
  tools/validate-specs.py               # exit 0 on clean, 1 on violations
  tools/validate-specs.py --quiet       # silent on success; for CI / pre-commit
  ```

Both scripts parse the canonical topic table from this document at
runtime, so adding a new row here (and nothing else) is sufficient to make
the scripts recognize the new topic.

### Examples

```
sys~mc_001~1        sys~ops_001~1        sys~persist_001~1    sys~arch_001~1
fw~mc_001~1         fw~est_001~1         fw~obs_001~1         fw~persist_001~1
fw~hal_spi_001~1    fw~hal_adc_001~1     fw~hal_gpio_001~1    (sub-topic IDs)
app~conn_001~1      app~views_001~1      app~obs_001~1
```

### Aside: smoketest

The smoketest in `tools/oft/_smoketest/` lives outside the real spec tree
(under `tools/`, never scanned by `oft trace specs/`) and uses descriptive
names (`sys~smoketest_motor_spins~1`, `fw~smoketest_pwm_init~1`) rather
than the topic-numbered convention. It is a test fixture, not a project
spec, and the descriptive names are clearer for that role.

## Spec template

A spec consists of: YAML frontmatter (`status`, `tags`), an `### H3`
heading, a `` `<type>~<topic>_<NNN>~1` `` ID line directly under the
heading, a one-paragraph "shall" statement, a testable `Acceptance:`
block, an explicit `Covers:` link to a parent spec (or a project-goal
reference for top-level `sys~` specs), and a `Needs:` declaration. No
boilerplate metadata blocks beyond that.

One **optional** block is allowed: a `Rationale:` section, placed after
the "shall" and before `Acceptance:`. It carries no OFT trace. When to use
it (rarely) and how to word it are governed by
[`spec-style.md`](spec-style.md).

Smallest possible spec:

````markdown
### Park transform
`fw~mc_001~1`

The FOC inner loop shall transform the measured three-phase stator currents
into the rotor-aligned (d, q) frame using the Park transform, with the
rotor electrical angle provided by the position estimator.

Acceptance:
- Computed (i_d, i_q) match a reference Python implementation to within
  numerical noise on the standard SIL trajectory.

Covers:
- sys~mc_001~1

Needs: impl, test
````

For full worked examples covering `fw~`, `app~`, and cross-component
`sys~` patterns plus the C / Rust / Python tag syntax, see
[`spec-template.md`](spec-template.md). If a spec needs more structure
than the smallest form, it is probably two specs.

**How the "shall" statement and acceptance bullets are *worded*** —
conciseness, stateless/end-state language, testable acceptance — is the
domain of [`spec-style.md`](spec-style.md). This document owns the
mechanics (IDs, trace, file layout); that one owns the prose. Read it
before writing spec language.

## Tooling: OpenFastTrace

[OpenFastTrace](https://github.com/itsallcode/openfasttrace) (OFT) is in
use, pinned at `4.2.2`, and installed project-locally under `tools/oft/`.
It:

- Is a single Java CLI (`oft`) — no service to run, fits cleanly in CI.
- Consumes Markdown specs and source-code comment tags directly. No foreign
  spec format to learn.
- Emits an HTML traceability report with coverage statistics and broken-link
  detection.
- Is language-agnostic (works equally for C/C++ firmware, Rust app code,
  Python SIL and notebook code).
- Is mature and used in industry-grade safety-critical projects.

Workflow:

1. Author or update a spec under the appropriate `specs/<component>/<topic>/`
   folder.
2. Write the test, tagging the spec: `// [test->fw~xxx~1]` (or `app~`,
   `sys~`).
3. Implement, tagging the implementation: `// [impl->fw~xxx~1]` (in C/C++)
   or `// [impl->app~xxx~1]` (in Rust).
4. CI runs `tools/oft/oft.sh trace specs/ <source dirs>` and fails on:
   - Any spec without downstream coverage.
   - Any spec without an upstream parent.
   - Any tag pointing at an ID that does not exist or has been
     version-bumped.

Alternatives that were evaluated and rejected:

- **StrictDoc** — better as a requirements *authoring* tool with prettier
  rendered output, but its custom `.sdoc` format is heavier than the
  Markdown-plus-tags approach. Worth a second look only if standalone
  published spec documents become a deliverable in their own right.
- **Sphinx-Needs** — only worth pulling in if Sphinx is the project's
  chosen documentation generator, which it is not.
- **Roll our own** — rejected. Recreating broken-link detection and a
  coverage matrix is not a good use of project time when OFT exists and is
  mature.

## Anti-bloat rules

The single biggest risk of any spec system is that it becomes a paperwork
graveyard. Rules to guard against that:

1. **Minimum viable spec.** One paragraph plus a testable acceptance block.
   No upfront long-form documents.
2. **Just-in-time authoring.** Specs are written when the code for them is
   about to be written, not as an upfront mega-document. The exception is
   top-of-tree `sys~` specs that anchor a subsystem as it begins development.
3. **Periodic audit.** Any spec that is not earning its keep — has no
   downstream code or tests, duplicates another spec, or has not been touched
   while the surrounding code has churned — gets merged or removed. A spec
   that exists "just for completeness" is noise.
4. **Resist the LLR temptation.** When tempted to add a low-level
   sub-requirement, first ask whether the parent spec plus clear code is
   actually insufficient. Usually it is not.
5. **One spec, one shall.** A spec containing "and ... and ... and ..." is
   really N specs. Split it.

## Open questions

- How specs interact with Jupyter notebooks: do notebooks embed `[test->...]`
  tags and participate in formal coverage, or are they treated as analysis
  artifacts that are referenced from specs but do not themselves provide
  coverage?
- Whether the hardware design itself participates in this spec system
  (e.g. `sys~` specs covered by hardware tests / bring-up checklists), or
  whether the hardware is treated as a fixed substrate with its own
  documentation outside this hierarchy.
- Concrete CI integration plan: where OFT runs, how reports are published,
  what failure modes block merges versus warn only.
