# Handoff: pcs_bldc bench cockpit

## Overview

A desktop tracing/telemetry cockpit for the `pcs_bldc` motor-control board. The operator picks a serial port, the app checks the board's reported firmware build against the loaded `.elf`, and then streams a watch list of firmware variables resolved from DWARF symbols. Live signals land on plot widgets and value tables in a rearrangeable workspace, with a cursor that is synchronized across every plot. A 3D pane renders the board and animates the rotor and its LED ring from the same telemetry.

The design's whole reason for existing is that **the device can refuse things**, and the UI has to be honest about it: budgets are shown before an action, refusals quote the firmware verbatim, missing samples are stated rather than interpolated, and a build-identity mismatch gates tracing while telemetry and logs keep flowing.

## About the design files

The files in this bundle are **design references authored in HTML**. They are prototypes that show intended look, layout and behavior — not production code to lift.

Your task is to **recreate these designs in the target codebase's existing environment**, using its established patterns, component library and state management. If the project has no UI environment yet, pick the framework that fits the product (for a desktop instrument like this, Tauri or Electron with React, or a native shell) and implement there.

Two things in the bundle are closer to implementable than the rest, and are called out where they appear:

- `rotor-view.js` — a real, working three.js web component. The geometry inside it is a stand-in, but the telemetry drive, LED model, theming bridge and lifecycle are the actual intended behavior and can be ported nearly as-is.
- `_ds/.../styles.css` — the real design-system token sheet. Take colors, type and spacing from it rather than from hex values transcribed by hand.

Ignore `support.js` — it is the prototype's own rendering runtime and has no place in production.

## Fidelity

**High-fidelity.** Colors, type, spacing, radii, elevation, iconography, states and copy are all final and intentional, and every value comes from a token. Recreate the UI faithfully.

Three deliberate exceptions, all visible in the prototype and labelled there:

1. Plot interiors are **empty dark rectangles**. No fake traces were drawn — the real implementation renders a canvas plotting library (uPlot is what the prototype names). Axis ticks, gridlines, the cursor line, the cursor readout and the gap ribbon are all designed and should be built as specified; only the trace paths are absent.
2. The 3D model is **stand-in geometry** traced from photographs of the board, marked `stand-in mesh` in the widget header. Swap in the exported assembly (see *3D pane* below).
3. The bottom-right **state switcher** (`.mock-switcher`) is a prototype affordance for viewing the four app states. Delete it.

---

## Design tokens

Every value in the design is a CSS custom property. **A theme is a token block and nothing else** — there are no per-component overrides anywhere in the design, and that property must survive the port. Three themes ship.

### Base ramps (from the Organic design system, `_ds/.../styles.css`)

The design system's ground is light cream; this app inverts onto the warm dark end of the same neutral ramp, because a bench instrument lives in a dim workshop. Use the design system's own ramps for text and accents.

| Token | Warm (default) | Graphite | Neon | Role |
| --- | --- | --- | --- | --- |
| `--color-neutral-200` | `#eee7db` | `#dfe4e7` | `#e7e2f5` | primary text |
| `--color-neutral-300` | `#dcd3c4` | `#bfc7cc` | `#c3bcd9` | secondary text, prose |
| `--color-neutral-500` | `#a19786` | `#8b959b` | `#8b83a6` | meta, units |
| `--color-neutral-600` | `#82796a` | `#69737a` | `#6a6383` | field labels |
| `--color-neutral-700` | `#645c50` | `#4a5257` | `#4a4463` | axis ticks, disabled, "no sample" |
| `--color-accent` | `#c67139` | `#2fa8bd` | `#e01fb8` | solid fills behind dark text |
| `--color-accent-300` | `#ffc6a5` | `#a8ecf5` | `#ffb3ee` | link hover |
| `--color-accent-400` | `#f6a06b` | `#5fd8e8` | `#ff6fe0` | accent on dark: cursor, attention, links |
| `--color-accent-500` | `#c48a5e` | `#2b95a8` | `#c018a0` | pressed, split-axis Y labels |
| `--color-accent-2-300` | `#ccdbb2` | `#c6e8cf` | `#b7f7e2` | text on healthy tint |
| `--color-accent-2-400` | `#aebf92` | `#8ed99f` | `#4ff0c0` | healthy: matched, accepted, ok |

### Surfaces (`--ink-*`, this app's addition)

| Token | Warm | Graphite | Neon | Used for |
| --- | --- | --- | --- | --- |
| `--ink-bg` | `#1b1916` | `#111315` | `#08070d` | app ground, workspace canvas |
| `--ink-chrome` | `#211f1b` | `#15181a` | `#0c0b14` | telemetry strip, signal picker, log pane |
| `--ink-surface` | `#262320` | `#1b1f22` | `#13101f` | connection bar, widgets, dialogs |
| `--ink-raised` | `#302c27` | `#242a2e` | `#1c1730` | inputs, hover, active rows, chip groups |
| `--ink-well` | `#131110` | `#0a0c0d` | `#040309` | plot canvas and 3D stage — the only true wells |
| `--ink-hint` | `#3d3831` | `#2c3338` | `#2a2340` | watermark text inside wells |
| `--ink-warn` | `#33261c` | `#22282b` | `#1f0f1c` | connection bar in the mismatch state |

### Overlay triples (`--*-rgb`)

Alpha overlays must retint per theme, so they are declared as RGB triples and consumed as `rgba(var(--hair-rgb), .1)`.

| Token | Warm | Graphite | Neon |
| --- | --- | --- | --- |
| `--hair-rgb` | `238,231,219` | `223,228,231` | `231,226,245` |
| `--lit-rgb` | `246,160,107` | `95,216,232` | `255,111,224` |
| `--accent-rgb` | `198,113,57` | `47,168,189` | `224,31,184` |
| `--ok-rgb` | `174,191,146` | `142,217,159` | `79,240,192` |
| `--surface-rgb` | `38,35,32` | `27,31,34` | `19,16,31` |
| `--bg-rgb` | `27,25,22` | `17,19,21` | `8,7,13` |
| `--well-rgb` | `19,17,16` | `10,12,13` | `4,3,9` |

Standard alphas in use: `.06`/`.07` row rules, `.1`/`.12` surface hairlines, `.14`/`.16`/`.18` control borders, `.22` resize handles, `.06`–`.14` tinted fills, `.78` gate scrim, `.96` readout backgrounds.

### Radius (`--r-*`)

Radii step down as elements get smaller and more functional. The plot canvas is deliberately the hardest-edged surface in the app — its squareness reads as "this is the measurement."

| Token | Warm | Graphite | Neon | Applies to |
| --- | --- | --- | --- | --- |
| `--r-widget` | `20px` | `3px` | `2px` | widget cards |
| `--r-dialog` | `26px` | `4px` | `2px` | dialogs, gate note |
| `--r-card` | `16px` | `2px` | `2px` | budget meter block, hints, drop targets |
| `--r-row` | `12px` | `2px` | `2px` | signal rows |
| `--r-canvas` | `10px` | `1px` | `0px` | plot canvas, 3D stage |
| `--r-pill` | `999px` | `3px` | `2px` | every control: buttons, chips, tags, meters |

The Organic design system pins `.btn` and `.tag` to pills; the two angular themes retune that with a scoped rule (`:root[data-theme=…] .btn, … .tag { border-radius: var(--r-pill) }`).

### Type

| Token | Warm | Graphite | Neon |
| --- | --- | --- | --- |
| `--font-display` | `"Caprasimo", system-ui, serif` | `"Bahnschrift Condensed", "Bahnschrift SemiCondensed", "DIN Condensed", "Avenir Next Condensed", "Arial Narrow", "Roboto Condensed", "Liberation Sans Narrow", system-ui, sans-serif` | `"Iosevka Term", "JetBrains Mono", "IBM Plex Mono", "SF Mono", ui-monospace, Menlo, Consolas, monospace` |
| `--display-track` | `normal` | `.09em` | `.13em` |
| `--display-case` | `none` | `uppercase` | `uppercase` |
| `--display-weight` | `400` | `600` | `700` |

Body face is `"Figtree", system-ui, sans-serif` in all themes. Mono is the platform stack `ui-monospace, Menlo, Consolas, monospace`.

**The rule that governs all type:** *every machine-authored string is mono; every human-authored string is the body or display face.* Values, symbol names, hashes, timestamps, port paths, units, log lines, firmware error strings — mono, tabular, right-aligned in numeric columns. Labels, prose, buttons, explanations — Figtree. The display face appears only as a title and never sits next to a number.

Sizes: display titles 13.5px (widget) / 17–18px (dialog) / 20px (empty state); body 12–15px; buttons 11.5–12.5px; field labels 9.5–10px uppercase at `.11em` tracking; mono 10.5px (axis ticks, pills) / 11–12px (values, log) / 14px (telemetry strip).

### Spacing

Organic's density scale rounded to an instrument grid: **4 · 6 · 8 · 12 · 16 · 20**. Widget padding 12; row padding 6/8; gutter between widgets 12; window padding 12–16; rows 26–30px tall.

### Elevation and texture

Two shadows only: floating widgets `0 3px 14px rgba(0,0,0,.32)`, overlays and dialogs `0 14px 40px rgba(0,0,0,.55)`. Every surface also carries a 1px `rgba(var(--hair-rgb),.1)` hairline — on a dark ground the hairline does the separating and the shadow only says "this floats and can be dragged."

Per-theme texture tokens, all `none` in Warm:

| Token | Graphite | Neon |
| --- | --- | --- |
| `--chrome-grad` | faint top sheen | magenta wash + mint side wash |
| `--surface-grad` | top sheen to 46% | magenta diagonal to 38% |
| `--well-grad` | 3px scanlines | scanlines + floor bloom |
| `--accent-grad` | `linear-gradient(180deg,#6fdff0,#25879a)` | `linear-gradient(135deg,#ff7ce4,#a04dff)` |
| `--cursor-glow` | `0 0 10px …, 0 0 26px …` | `0 0 12px …, 0 0 34px …` |
| `--chip-glow` | `inset 0 1px 0 rgba(var(--hair-rgb),.12)` | `0 0 12px rgba(var(--accent-rgb),.35)` |
| `--edge-lit` | `rgba(var(--hair-rgb),.09)` | `rgba(var(--lit-rgb),.22)` |

`--edge-lit` is applied as `border-top-color` on widgets and reads as a light catching the top edge. Warm sets it `transparent`.

### Icons

Lucide, with weight and cap as theme tokens: `--icon-stroke` `2.75` / `2` / `1.6` and `--icon-cap` `round` / `square` / `square` (Warm / Graphite / Neon). One set of paths, three voices. Icons in use: search, grip-vertical (drag handle), more-horizontal (widget menu), chevron-left/right (readout pager), chevron-down (log collapse), crosshair (cursor time), activity (link rate), alert-triangle (mismatch), terminal (log), plus (add widget / drop target).

### Semantic color rules

Only two accents carry meaning, and they carry it consistently:

- **sage / mint (`--color-accent-2-*`)** — the device agrees with us: identity matched, watch list accepted, budget within limits, boolean false on a fault flag.
- **terracotta / cyan / magenta (`--color-accent-*`)** — attention, or the live cursor.

**There is deliberately no red.** A refusal or a mismatch is not a crash — the board is doing its job — so the accent, at higher border weight, does the raising. Reserve any future red strictly for a latched hardware fault. New state colors come from a ramp step, not a new hue: two accents is the budget.

### Motion

**Nothing animates except live data and the 10 Hz link dot.** No transitions on layout, no easing on state changes — a moving instrument panel is a lying instrument panel. The two exceptions:

- `pulseDot` — 2s ease-in-out infinite opacity 1 → .35 → 1 on the link-rate icon.
- The 3D pane's render loop, and the camera's ~6/sec lerp toward a view preset.

Scrollbar chrome is suppressed app-wide (`scrollbar-width: none` + `::-webkit-scrollbar { display: none }`): a bench panel shows data, not tracks.

---

## Layout

A five-band vertical stack filling the window, fluid at any size:

```
┌────────────────────────────────────────────────────────┐
│ .conn-bar          (flex none, wraps)                  │
├────────────────────────────────────────────────────────┤
│ .telemetry-strip   (flex none, scrolls horizontally)   │
├──────────────┬─────────────────────────────────────────┤
│ .signal-     │ .workspace                              │
│  picker      │   .widget-grid  (scrolls)               │
│  (302px)     │   or .workspace-empty                   │
│              │   + .trace-gate / .reject-scrim overlay │
├──────────────┴─────────────────────────────────────────┤
│ .log-pane          (flex none, max-height 34vh)        │
└────────────────────────────────────────────────────────┘
```

**Flex discipline that matters.** Every chip in the horizontal chrome rows is `flex: none` with `white-space: nowrap`, and the rows wrap (`flex-wrap: wrap; row-gap: 8px`) rather than clip. This was arrived at by fixing real bugs: with default `flex-shrink: 1` the rows squeeze chips below their text width and the text wraps mid-value; with `overflow: hidden` instead, the recovery button on the right gets clipped off-screen exactly when the user needs it. Wrapping is the correct behavior — every chip stays whole and the `.conn-elf` group drops to a second line at narrow widths.

`.workspace` is `overflow: hidden` and its children own their own scrolling, so an oversized empty state cannot paint over the log pane.

### Region map

| Class | What it is |
| --- | --- |
| `.app-shell` | window root, owns the vertical stack |
| `.conn-bar` | `+ --matched` / `--mismatch`; children `.conn-port` `.conn-identity` `.conn-elf` `.conn-gate-note` `.theme-switch` |
| `.telemetry-strip` | 10 Hz cells, one `.telemetry-cell` each |
| `.signal-picker` | `.picker-search` `.budget-meters` `.budget-meter` `.picker-tree` `.picker-group` `.signal-row` `.period-seg` `.period-pill` `.picker-hint` |
| `.workspace` | canvas; contains `.widget-grid` or `.workspace-empty` |
| `.plot-widget` | `+ --split`; `.widget-head` `.axis-mode` `.widget-legend` `.plot-body` `.y-axis` `.plot-canvas` `.gap-ribbon` `.x-axis` `.resize-handle` |
| `.cursor-line` | one per plot, driven by the shared cursor time |
| `.cursor-readout` | `+ --flipped` when the cursor is past mid-plot |
| `.table-widget` | `.value-table` `.drop-target` |
| `.rotor-widget` | `.rotor-body` `.rotor-stage` `.rotor-readout` `.led-pattern-chips` `.spin-chips` `.view-gizmo` `.pose-held`; the canvas is `<rotor-view>` |
| `.trace-gate` | identity scrim + `.trace-gate-note` |
| `.reject-dialog` | watch-list refusal; holds the firmware reason string verbatim |
| `.log-pane` | `.log-lines`; collapsible and resizable from its header |
| `.mock-switcher` | **prototype only — delete** |

---

## Screens and components

### 1. Connection bar

`flex: none`, `padding: 10px 16px`, `gap: 14px`, `row-gap: 8px`, wraps. Background `--ink-surface` + `--chrome-grad`, bottom border `1px rgba(var(--hair-rgb),.1)`.

Left to right:

- **App mark** — `pcs_bldc`, display face, 17px, `--color-accent-400`, `letter-spacing: -.01em`.
- **`.conn-port`** — pill, `--ink-raised`, 1px `rgba(var(--hair-rgb),.12)`, `padding: 5px 6px 5px 12px`. A 7px sage dot with a `0 0 0 3px rgba(var(--ok-rgb),.16)` halo, the port path in mono 12.5px, and a secondary **Disconnect** button (11.5px, `padding: 2px 10px`, transparent, `rgba(var(--hair-rgb),.18)` border).
- **`.conn-identity`** — the build check. Matched state: pill, `rgba(var(--ok-rgb),.1)` fill, `rgba(var(--ok-rgb),.3)` border, holding `MATCHED` (10px uppercase, `.1em`, 700, sage), a 1px 14px divider, the build string in mono (`g431-v0.9.4 · 8f31c0a`), and `704/704 symbols resolve` in 11.5px `--color-neutral-500`.
- **`.conn-elf`** — right-aligned via `margin-left: auto`. Label `elf` (10px uppercase), the path in mono, and a **Reload** button.
- **`.theme-switch`** — three buttons in a pill group on `--ink-raised`. Each has a swatch dot (circle in Warm, 1px-radius square in the angular themes) and a label; the selected one gets a `rgba(var(--hair-rgb),.25)` 2px ring on the dot and a `rgba(var(--hair-rgb),.12)` background.

**Mismatch variant** (`.conn-bar--mismatch`): background `--ink-warn`, bottom border `rgba(var(--lit-rgb),.42)`. `.conn-identity--mismatch` uses `rgba(var(--lit-rgb),.14)` fill and a solid `--color-accent-400` border, an alert-triangle icon before the word `MISMATCH`, both hashes side by side (`board 8f31c0a · elf 1d77b2e`), and `tracing off`. A `.conn-gate-note` follows with *"Telemetry and logs keep flowing."* and a **Load the matching .elf** link. `.conn-elf` swaps Reload for a primary **Choose…** button.

### 2. Telemetry strip

`flex: none`, `padding: 0 16px`, background `--ink-chrome` + `--chrome-grad`, `overflow-x: auto`, `white-space: nowrap`. Every cell is `flex: none` with `padding: 8px 16px` and a `1px rgba(var(--hair-rgb),.08)` left divider (first cell has none). Each cell is a 9.5px uppercase `.11em` label over a 14px mono value.

Cells, in order: **link** (activity icon + `10 Hz`, sage, pulsing) · **drive state** (`RUN · FOC_VEL`) · **v‌bus** (`19.84 V`) · **i‌bus** (`1.271 A`) · **velocity — cmd / meas** (`1400.0 / 1387.4 rpm`, with the measured value in accent and the slash in `--color-neutral-700`) · **tick** (`184 209 ms`) · then right-aligned tags `14 signals watched` (accent tint) and `3 gaps` (neutral tint).

### 3. Signal picker

`width: 302px`, `flex: none`, `overflow: hidden`, background `--ink-chrome` + `--chrome-grad`, right border hairline.

- **Header** — `Signals` in the display face at 15px, and `704 from DWARF` in 11px mono `--color-neutral-600`.
- **`.picker-search`** — pill on `--ink-raised` with a `rgba(var(--lit-rgb),.55)` border when focused, a search icon, the query in mono, a 1px accent caret, and a right-aligned result count.
- **`.budget-meters`** — a `--ink-surface` card, `--r-card`, `padding: 11px 13px`. Header row: `DEVICE BUDGET` (9.5px uppercase) and status (`accepted`) in mono. Then two `.budget-meter` rows, each a label + `used / max` in mono over a 5px track (`rgba(var(--hair-rgb),.1)`) with a pill fill. **watch RAM** `412 / 512 B` at 80.5% in sage; **link bandwidth** `88 / 100 %` at 88% in accent + `--accent-grad` + `--chip-glow`. The color crossover is the point: sage while comfortable, accent as it approaches the limit.
- **`.picker-tree`** — scrolls. `.picker-group` headers are the full symbol path in 11px mono sage, followed by a hairline rule that fills the remaining width. `.signal-row` is `padding: 6px 8px`, `--r-row`, `cursor: grab`, `draggable`: an 8px 2px-radius color chip (the trace color, or `--color-neutral-700` when unwatched), the name in 12px mono with ellipsis, the C type in 10px, and either a `.period-pill` (`1 ms`, sage tint) or the word `add` in `--color-neutral-700`. Watched rows carry a `rgba(var(--lit-rgb),.1)` background. The active row is on `--ink-raised` with a `0 0 0 1px rgba(var(--lit-rgb),.45)` ring and shows `.period-seg` — a three-way `1 / 10 / 100` segmented control, selected segment filled with `--color-accent` + `--accent-grad`.
- **`.picker-hint`** — dashed `rgba(var(--hair-rgb),.16)` border, `--r-card`, 11.5px `--color-neutral-500`: *"Drag a signal onto a plot to trace it, or onto a table for latest value. Changing the watch list restarts the stream."*

### 4. Workspace and plot widgets

`.widget-grid` is `display: grid; grid-template-columns: repeat(auto-fit, minmax(420px, 1fr)); grid-auto-rows: minmax(248px, 1fr); gap: 12px; padding: 12px; overflow: auto`. The workspace canvas itself carries a 22px dot grid: `radial-gradient(rgba(var(--hair-rgb),.07) 1px, transparent 1px)`.

Every widget: `min-height: 248px`, `--ink-surface` + `--surface-grad`, 1px hairline with `border-top-color: var(--edge-lit)`, `--r-widget`, `0 3px 14px rgba(0,0,0,.32)`, `overflow: hidden`, `position: relative`.

**`.widget-head`** is the family resemblance and every widget type inherits it whole: `flex-wrap: nowrap`, `gap: 10px`, `overflow: hidden`, `padding: 9px 12px 8px`, bottom hairline `rgba(var(--hair-rgb),.07)`, `cursor: move`. Contents in fixed order: a 14px grip icon in `--color-neutral-700` (`margin-left: -3px`), the title in the display face at 13.5px nowrap, an `.axis-mode` segmented control, optional tags, then `.widget-legend` pushed right with `margin-left: auto` and `flex: none` — per-signal entries (an 8×2.5px color bar, the name in 11px mono, the period in `--color-neutral-700`) and finally a 16px more-horizontal icon.

**`.axis-mode`** — a two-option pill group (`shared Y` / `split`), 1px `rgba(var(--hair-rgb),.14)` border, `overflow: hidden`; selected option filled `--color-accent` + `--accent-grad` with `--ink-bg` text, unselected `--color-neutral-500` on transparent with a 1px left divider.

**`.plot-body`** — `flex: 1; min-height: 96px; padding: 8px 12px 4px`. A 44px `.y-axis` column (right-aligned 10.5px mono ticks in `--color-neutral-600`, `justify-content: space-between`), then `.plot-canvas`.

**`.plot-canvas`** — `--ink-well` + `--well-grad`, `--r-canvas`, `overflow: hidden`. Contains, in layers:
- gridlines: `linear-gradient(rgba(var(--hair-rgb),.055) 1px, transparent 1px) 0 0/100% 25%` for horizontals, `linear-gradient(90deg, rgba(var(--hair-rgb),.04) 1px, transparent 1px) 0 0/12.5% 100%` for verticals
- a brighter `rgba(var(--hair-rgb),.12)` zero line where the scale crosses zero
- `.gap-ribbon` — a 4px `rgba(var(--hair-rgb),.04)` strip pinned to the bottom edge, with accent-colored spans marking dropped-sample windows. **This is how missing data is shown on a plot; traces are never bridged across a gap.**
- a `--ink-hint` watermark naming the renderer (`canvas · uPlot`)
- `.cursor-line` — 1px `--color-accent-400` full height with `box-shadow: var(--cursor-glow)`
- `.cursor-readout` — see below

**`.x-axis`** — 10.5px mono `--color-neutral-700`, `justify-content: space-between`, left padding aligned past the Y-axis column (68px normally, 76px on the split-axis widget which also needs 58px on the right).

**`.resize-handle`** — 12px corner bracket at `right: 5px; bottom: 5px` built from `border-right` + `border-bottom` at `2px rgba(var(--hair-rgb),.22)` with a 4px corner radius, `cursor: nwse-resize`.

**The three plots in the populated state:**

| Widget | Signals | Axes |
| --- | --- | --- |
| Phase currents | `ia` `ib` `ic` @ 1 ms | shared Y, ±4 A |
| Phase voltages | `va` `vb` `vc` @ 1 ms | shared Y, 0–20 V |
| Rotor velocity | `cmd` @ 10 ms, `meas` @ 1 ms, `err` @ 1 ms | **split**: rpm left (1000–1600, labels in `--color-accent-500`), error right (−20…+40, labels in `--trace-4`) |

The split-axis widget gets a second `.y-axis` on the right (46px, left-aligned, `padding-left: 7px`) and names two scales in its watermark.

### 5. Cursor and its readout

One cursor time is app state, shared by every plot. All plots draw `.cursor-line` at the same time value, and each shows its own `.cursor-readout`.

`.cursor-readout`: 186–196px wide, `padding: 8px 10px`, `rgba(var(--surface-rgb),.96)`, 1px `rgba(var(--lit-rgb),.35)`, `--r-row`, `0 6px 18px rgba(0,0,0,.5)`, positioned `left: calc(<cursor%> + 10px); top: 10px`. Header row: a crosshair icon + `t = 12 480 ms` in 11px mono accent, and a pager on the right. Body: one row per signal — color bar, name in 11.5px mono `--color-neutral-300`, value right-aligned in 12px mono `--color-neutral-200`.

**`--flipped`** — when the cursor is past mid-plot the readout flips to the left side (`right: calc(<100-cursor%> + 10px)`) so it never leaves the canvas.

**Pagination.** With many signals the readout does not grow or scroll: it **paginates by signal group, one page per Y scale**. The pager shows `‹ 2 / 2 ›` with chevron icons, and the page carries a 9.5px uppercase group label (`error · scale 2`). Single-scale plots show a static `1 / 1`.

**Missing samples** are written as the literal text `no sample` in `--color-neutral-700`, with the signal's color chip also dropped to `--ink-hint`. Never interpolate, never show a stale number as if it were current.

### 6. Table widget

Head carries the title `Live values` and a neutral tag `at cursor · 12 480 ms`. Body scrolls.

`.value-table` — `width: 100%; border-collapse: collapse`, 12px mono. Header cells are 9.5px uppercase `.11em` Figtree 600 in `--color-neutral-600` with a `1px rgba(var(--hair-rgb),.12)` bottom border; body cells `padding: 6px 8px` with `1px rgba(var(--hair-rgb),.06)` row rules (last row none).

Four columns: **signal** (left; a 7px color chip inline, 7px right margin, then the symbol path), **value** (right-aligned — numerics right-align so digits line up), **type** (left, `--color-neutral-600`), **period** (right, `--color-neutral-500`).

Value coloring follows meaning, not type: numbers `--color-neutral-200`, an enum `--color-accent-400`, a boolean `false` on a fault flag `--color-accent-2-300` (sage, because false is the healthy answer there).

Below the table, a `.drop-target`: dashed border, `--r-card`, centered 11.5px `--color-neutral-700`, *"drop a signal to add a row"*.

### 7. 3D rotor pane

Head: grip, title `Rotor assembly`, an `.axis-mode`-styled `live` / `at cursor` toggle, a neutral tag `stand-in mesh`, and a legend showing `rotorA 1ms` in accent and `rotorB target` in `--color-neutral-700`.

`.rotor-stage` — `--ink-well` + `--well-grad`, `--r-canvas`, `overflow: hidden`, with the `<rotor-view>` canvas absolutely filling it and four overlays:

- **`.rotor-readout`** top-left, `pointer-events: none`: three rows of `label` (34px column, `--color-neutral-600`) + value in 11.5px mono — `θm`, `θe`, and `rpm` (accent). Updated at 10 Hz by writing `textContent` directly. **Do not route this through app state** — a 10 Hz readout must not re-render the app.
- **`.led-pattern-chips`** bottom-left: a `LED ARC` label plus five chips. Selected chip is `rgba(var(--lit-rgb),.2)` with accent text; unselected `rgba(var(--surface-rgb),.8)` with `--color-neutral-600`.
- **`.spin-chips`** just above: a `SPEED` label plus `jog 45` / `run 1387`.
- **`.view-gizmo`** bottom-right: `iso` / `top` / `front` in a pill group on `rgba(var(--surface-rgb),.86)`.
- **`.pose-held`** top-right, only when stale: accent-tinted pill reading `pose held · no sample`.

Footer strip: `channels[0] · 14 pole pairs · 36 RGB at 10° · arc smears above ~200 rpm, as on the bench` and `drag to orbit · scroll to zoom`.

See **The 3D pane** section below for the behavior.

### 8. Log pane

`flex: none`, `max-height: 34vh`, background `--ink-chrome` + `--chrome-grad`, top hairline `rgba(var(--hair-rgb),.1)`.

Header (`padding: 7px 16px`, `cursor: ns-resize`): a terminal icon + `FIRMWARE LOG` (10px uppercase `.11em`), a sage tag `printf · live`, then right-aligned `112 lines`, `clear`, and `collapse ⌄` with a chevron.

`.log-lines` — `height: clamp(58px, 13vh, 104px)`, scrolls, 11.5px mono, `line-height: 1.75`, `--color-neutral-500`. Each line is a `--color-neutral-700` timestamp, a severity glyph (`i` sage, `w` accent), then the message. Log text is firmware output and is never reformatted or paraphrased.

### 9. Empty workspace

`flex: 1; min-height: 0; overflow: auto`, centered column, `gap: 18px`, `padding: 40px`.

A 340×150px drop box: 1.5px dashed `rgba(var(--hair-rgb),.2)`, `--r-widget`, `rgba(var(--well-rgb),.5)` fill, holding a plus icon + `drop a signal here` in 12px mono `--color-neutral-600`. Below, a 420px centered block: `An empty bench` in the display face at 20px, then *"Drag any of the 704 firmware variables from the left onto the canvas. A plot appears where you drop it; drop onto an existing plot to overlay."* Then three buttons: primary **New plot widget** (with a plus icon), secondary **New table widget** (plus icon), secondary **Load layout…**.

---

## States

Four app states, all reachable from the prototype's switcher.

### `matched` — the default

Everything streams. Connection bar sage, plots live, budgets accepted.

### `mismatch` — the identity gate

The board's reported build does not match the loaded `.elf`. Symbol addresses from a different build would read the wrong memory, so **the watch list is held and tracing is disabled — but telemetry and logs keep flowing.**

The design brief for this state was "unmistakable but not alarmist," and it resolves in three coordinated moves: the connection bar turns to its warning variant, a scrim dims the workspace, and a single inline explanation says why.

`.trace-gate` — `position: absolute; inset: 0`, `rgba(var(--bg-rgb),.78)`, `backdrop-filter: saturate(.4)`, content aligned to `padding-top: 14vh`. `.trace-gate-note` is `min(520px, 86%)` wide, `--ink-surface` + `--surface-grad`, 1px `rgba(var(--lit-rgb),.4)`, `--r-dialog`, `0 14px 40px rgba(0,0,0,.55)`, `padding: 18px 20px`, `gap: 10px`:

- alert-triangle icon + title *"Tracing is paused — build identity doesn't match"* (display face, 17px)
- body, 13px `--color-neutral-300`: *"The board reports `g431-v0.9.4 · 8f31c0a`; the loaded .elf is `1d77b2e`. Symbol addresses from a different build would read the wrong memory, so the watch list is held. Telemetry and the log keep streaming."* (hashes in mono `--color-accent-300`)
- primary **Choose matching .elf…**, secondary **Keep layout, trace anyway**
- footer, 11px `--color-neutral-600`: *"Layout and watch list are preserved — tracing resumes the moment identities agree."*

The rotor pane holds its pose and shows `pose held · no sample`; PD-FLT blinks on the 3D board.

### `rejected` — the device refused the watch list

`.reject-scrim` is `rgba(var(--bg-rgb),.6)`, centered. `.reject-dialog` is `min(460px, 92%)`, `--r-dialog`, 1px `rgba(var(--lit-rgb),.4)`, `padding: 20px 22px`, `0 18px 48px rgba(0,0,0,.6)`:

- title *"Device refused the watch list"* (display face, 18px)
- **the firmware's reason, verbatim**, in a `--ink-bg` block with `--r-card` and a 2px `--color-accent-400` left border, under a 9.5px uppercase label `REASON FROM FIRMWARE`, text in 12px mono `--color-accent-300`:
  `E_WATCH_BUDGET: 17 entries at 1 ms needs 129 % of link (max 100 %); RAM 596 / 512 B`
- reassurance, 13px: *"The previous list is still running — nothing was lost. Drop a signal, or move some to a slower period."*
- two suggested fixes, each a sage bullet with the resulting budget: *"Move the 6 raw ADC signals to 10 ms → 71 % of link"*, *"Drop `est_flux_data.thetaElRaw` → 94 %"*
- primary **Apply first fix**, secondary **Edit watch list**

**Never paraphrase a firmware error.** Quote it, then explain around it.

### `empty` — no widgets

The empty workspace described above.

---

## Interactions

Designed and specified, mostly not wired in the prototype. Build all of these.

| Interaction | Behavior |
| --- | --- |
| Port connect | Pick port → connect → read build identity → compare to loaded `.elf` → enter `matched` or `mismatch` |
| Signal search | Filters the DWARF symbol list; result count in the search pill |
| Drag signal → plot | Adds a trace to that plot, assigned the next color in the cycle |
| Drag signal → empty canvas | Creates a plot widget where it was dropped |
| Drag signal → table | Appends a row |
| Period assignment | Per-signal `1 / 10 / 100` ms; changing it recomputes the budgets live, before the request goes out |
| Watch-list commit | Any change restarts the stream (`tick base = 0`, logged). If the device refuses, the previous list keeps running and `rejected` is shown |
| Cursor hover | Sets the app's cursor time; every plot's `.cursor-line` and `.cursor-readout` follow, and the table switches to values at that time |
| Readout pager | Steps through signal groups, one page per Y scale |
| Widget drag / resize | Reorder and resize within the grid; layout persists |
| Axis mode | Toggles a plot between one shared Y scale and per-group split scales |
| Log collapse / resize | Header is the drag handle; collapse leaves the header row visible |
| Theme switch | Sets `data-theme` on the document root. Nothing else changes |
| 3D orbit | Pointer drag orbits (azimuth ±, elevation clamped 0.06–1.5 rad), wheel zooms (distance clamped 70–260), view chips lerp the camera to a preset |

**Focus and hover** must be themed, never browser defaults: `:focus-visible { outline: 2px solid var(--color-accent-400); outline-offset: 2px }`, and every interactive element gets a hover tint from the accent ramp. Links are `--color-accent-400`, hover `--color-accent-300`.

---

## State management

| State | Type | Notes |
| --- | --- | --- |
| `port`, `connected` | string, bool | |
| `boardIdentity`, `elfPath`, `elfHash` | strings | `identityMatched` is derived, and gates tracing |
| `symbols` | DWARF symbol table | 704 entries in the mock |
| `watchList` | `[{ symbol, period, color }]` | committing restarts the stream |
| `budgets` | `{ ramUsed, ramMax, linkUsed, linkMax, accepted, reason }` | `reason` holds the verbatim firmware string |
| `layout` | widget list with type, position, size, signals, axis mode | persisted |
| **`cursorTime`** | number \| null | **app-level, not per-widget** — this is what makes the cursor synchronized and lets the 3D pose follow it |
| `readoutPage` | per widget | index into that plot's scale groups |
| `theme` | `warm` \| `graphite` \| `neon` | writes `data-theme` on the root |
| `logLines` | ring buffer | |
| 3D pane | `mode`, `view`, `ledPattern`, `spin` | |

Sample data is high-rate; the plot store and the 3D pane both need decimation to display rate. Never render per sample.

---

## The 3D pane

`rotor-view.js` is a working three.js web component and the closest thing here to production code. It is a plain custom element (`<rotor-view>`) with no framework dependency, loaded as an ES module; three.js is pulled in via dynamic `import()` and failure is handled by showing *"3D pane unavailable — renderer could not load. Plots and telemetry are unaffected."* rather than breaking the pane.

### Attributes

`mode` (`live` \| `cursor`), `view` (`iso` \| `top` \| `front`), `pattern`, `rpm`, `cursor-ms`, `stale`, `fault`, `theme-key`. It accepts both kebab and flat spellings (`cursor-ms` and `cursorms`) because React-mounted custom elements receive attributes lowercased and hyphen-free.

### Events

Emits `pose` at 10 Hz, bubbling and composed, with `{ mech, elec, rpm, live }`.

### Replacing the geometry

**STEP cannot be loaded by three.js** — it is boundary-representation CAD (surfaces and solids) and a renderer needs triangle meshes. Two routes: export glTF/GLB from CAD (cleanest — you control decimation and node naming), or tessellate in-browser with an OpenCascade wasm build (handles STEP directly, but several MB and slow on a full assembly).

Swap `buildScene()` for a `loadGLTF()` and everything else in the file keeps working, provided the model carries these named nodes:

```
board
rotorA        driven — the large rotor
rotorB        the encoder test target — static, nothing drives it
ledRing
led_D30 … led_D65      36 RGB ring LEDs, 10° pitch
led_PD_FLT  led_5V0  led_3V3  led_VBUS  led_HEART
```

Strip the assembly first — every screw and pin header is far more geometry than the pane needs. Rotor, stator, shaft, board outline, connectors is enough.

### Telemetry drive

Rotor angle comes from `est_flux_data.thetaEl` divided by pole pairs (**14**). Two modes:

- **live** — integrates the reported speed at display rate. Never a redraw per sample.
- **at cursor** — the pose is a pure function of `cursorTime`, so scrubbing a fault in the plots scrubs the machine. This is the pairing that makes the pane more than decoration.

Stale data holds the last pose and says so. It never interpolates. The loop early-returns on `document.hidden` so a background window costs nothing.

### The LED model

**Ring:** 36 RGB LEDs, closed ring, 10° pitch, with long silkscreen ticks at the quadrants. The firmware's pattern is **position** — one LED tracks mechanical angle — which is the default. Four alternates ship in the file for comparison (`comet`, `commutation`, `electrical`, `sweep`); keep or drop them as you like, but `position` is the real one.

Two details do the visual work and are worth preserving:

1. **Persistence.** Every LED runs its target through a rise/fall filter — 28 ms up, 85 ms down — because an LED does not switch, it charges and decays. This is what makes the ring smear at speed instead of strobing at frame rate.
2. **Spill.** Each LED is an emissive package *plus* a flat additive disc lying on the PCB. Without the spill an emissive box reads as white paint, not light.

**Status LEDs** follow a different rule, because they report state rather than motion: rails (`5V0`, `3V3`, `VBUS`) solid when up, `PD_FLT` blinking only on fault, and a heartbeat at the link rate.

**Known honest limitation:** above roughly 200 rpm the ring is a uniform glow. At 1387 rpm the rotor turns 23 times a second and no pattern is legible — that is what the bench actually looks like, so the pane keeps it and offers a jog speed for reading the pattern.

### Theming

The pane reads `--color-accent-400` and `--color-accent-2-400` off the document root and watches `data-theme` with a `MutationObserver`, so it restyles with the rest of the app. Emissive gain is raised in Neon.

---

## Trace colors

A six-color cycle, assigned in order as signals are added and **remembered per signal**, so a trace keeps its color when it moves between widgets. The first two alias the theme's accents; the other four sit at the same OKLCH lightness (~0.78) so no trace visually outweighs another.

| | Warm | Graphite | Neon |
| --- | --- | --- | --- |
| `--trace-1` | `--color-accent-400` | `--color-accent-400` | `--color-accent-400` |
| `--trace-2` | `--color-accent-2-400` | `--color-accent-2-400` | `--color-accent-2-400` |
| `--trace-3` | `#e0c46a` | `#e8c46a` | `#ffd84f` |
| `--trace-4` | `#86b8c4` | `#9d8bf0` | `#5fd8ff` |
| `--trace-5` | `#d98a9c` | `#f07aa8` | `#a56fff` |
| `--trace-6` | `#c9c1b4` | `#b9c2c7` | `#c3bcd9` |

Beyond six, repeat the cycle with a dashed stroke rather than adding hues.

---

## How to extend the design

Rules that keep new surfaces consistent:

- A new widget type inherits `.widget-head` whole — display-face title, mode segmented control, legend right, `⋯` last. That header is the family resemblance.
- Anything the device can refuse gets a budget meter **before** the action and a verbatim reason string **after** it.
- Missing data is stated, never interpolated: `no sample` in `--color-neutral-700` in readouts and tables, an accent tick in the `.gap-ribbon` on plots, a held pose in the 3D pane.
- New state colors come from a ramp step, not a new hue. Two accents is the budget. No red except a latched hardware fault.
- Nothing animates except live data and the link dot.
- A fourth theme is a copy of one token block with new values — never a per-component override.

## Assets

No image or font files. Icons are Lucide paths inlined as SVG, drawn with `stroke-width: var(--icon-stroke)` and `stroke-linecap: var(--icon-cap)`. Caprasimo and Figtree come from the Organic design system's stylesheet; the angular themes' display faces are platform fonts with fallback chains, so nothing is fetched at runtime. three.js loads from `https://esm.sh/three@0.169.0` — pin and vendor this for production.

The 3D geometry is stand-in, modelled from three photographs of the board supplied during the design session.

## Open questions

Five things had no spec to point at and were designed on judgment:

1. **Workspace layout** — widget create, drag, resize and persistence.
2. **Shared cursor time as app-level state** — the mechanism is designed but the ownership and update path are not specified.
3. **The table widget** — columns are settled (name, value, type, period); sorting, grouping and row limits are not.
4. **Split axes** — how signals group into scales. The paginated cursor readout assumes one page per Y scale, which is the assumption most likely to need revisiting.
5. **The RGB ring** — it is RGB and nothing yet says what color encodes. It currently takes the theme accent throughout.

## Files

| File | What it is |
| --- | --- |
| `Cockpit.dc.html` | The full cockpit: all five bands, four widget types, four app states, three themes. Open it in a browser. |
| `Design notes.dc.html` | The design-system note — palette, trace cycle, type rules, spacing/radius/elevation, theme token map, region ids, extension rules. |
| `rotor-view.js` | The three.js pane. Real, working, portable. |
| `_ds/organic-…/styles.css` | The Organic design-system token sheet and component layer. **Source of truth for colors, type and spacing.** |
| `_ds/organic-…/_ds_bundle.js` | The design system's component bundle. |
| `_ds/organic-…/readme.md` | The design system's own guide. |
| `support.js` | Prototype rendering runtime. **Ignore — not for production.** |

The two `.dc.html` files open directly in a browser. Both are single files with inline styles; there is no build step and no stylesheet to hunt down.
