# Spec writing style

The canonical style guide for **how spec language is written** in this
project. Companion to [`spec-system.md`](spec-system.md), which owns the
*mechanics* (artifact types, IDs, file organization, OFT trace) — this doc
owns the *prose*: how the "shall" statement and acceptance bullets are
phrased so every spec reads consistently and tightly.

When the two docs overlap, `spec-system.md` is authoritative on mechanics
and this doc is authoritative on wording. The `pcs_spec` skill
(`.claude/skills/pcs_spec/SKILL.md`) drives the authoring workflow and
defers to this doc for all style questions.

## The rules

### 1. One spec, one shall

A spec states exactly one requirement. If the "shall" paragraph contains
"and ... and ...", or the acceptance block is testing two unrelated
behaviors, it is two specs. Split it.

### 2. Concise — every word earns its place

Spec language is reference text that gets read many times. Cut anything
that doesn't change the meaning:

- No throat-clearing ("In order to", "It should be noted that", "The
  purpose of this requirement is to").
- No restating the heading or the module name in the body.
- No hedging ("generally", "typically", "as appropriate", "where
  possible") — a requirement is definite or it isn't a requirement.
- Prefer the short word and the active voice.

### 3. State the single chosen behavior — no alternatives

A spec states what the system does, never what it does *instead of*
something else. Drop "X rather than Y", "X as opposed to Y", "X (not Y)"
constructions — the rejected alternative is noise. The "single detail of
behavior" is the chosen one.

> ✗ "...using a timeout computed from the transfer length rather than a
>    configured constant."
> ✓ "...enforcing a per-transfer timeout $t = \lceil ... \rceil$."

### 4. Affirmative — say what the system is, not what it is not

Describe the system's own behavior and properties in the positive. Do not
describe what the system *isn't*, what it *doesn't* do, or what another
layer or component handles instead. Negative or delegational framing states
something about the world *outside* this spec's subject — it belongs in
that subject's spec, or nowhere.

(Distinct from rule 3: rule 3 forbids naming the rejected *alternative* to
the chosen behavior; this rule forbids framing the requirement itself as a
negation or a hand-off to elsewhere.)

> ✗ "Device-specific knowledge (the AS5048 encoder, …) lives in `IO_*`
>    drivers one layer up." / "...not a property of an individual device."
> ✓ "Presents a byte-transfer API addressed by logical channel, usable by
>    any consumer."

### 5. No rationale in the requirement — state the behavior, not the why

A "shall" states *what* the system does, not *why*. Cut justifying clauses
— "so that…", "to enable…", "in order to…", "this allows…" — that explain
the motivation for the requirement. They balloon the prose with detail no
test checks, and the requirement stands on its own without them.

> ✗ "...report completion through a callback and a pollable status, so
>    consumers can integrate with the synchronization mechanism of their
>    choice."
> ✓ "...report completion through a callback and a pollable status."

**Most specs need no rationale at all** — their reason for existing is
implicit in what they require, and a reader can see why they are the way
they are. Adding a "why" to an obvious spec is noise.

The narrow exception: a **subjective** spec, where several genuinely viable
alternatives existed and the choice between them was a judgement call worth
recording so it isn't lost. *Only* such a spec may carry the **optional
`Rationale:` section** (see the structure note below). It is an escape hatch
for capturing valuable rationale that would otherwise vanish — not a slot to
fill. When in doubt, omit it; the spec's existence is its own rationale.

### 6. Spec only what matters — don't over-constrain

State what must be true; do not forbid implementation freedom that no
requirement needs. If nothing demands that a property be fixed, bounded, or
done a particular way, the spec stays silent and leaves it to the
implementation. Over-constraining bakes an incidental implementation choice
into the requirement and makes the spec lie the day the code legitimately
does otherwise.

> ✗ "Each bus shall operate in exactly one of three modes, fixed at
>    configuration." (forbids a runtime mode switch nothing rules out)
> ✓ "Each bus shall operate in one of the following modes:"

### 7. Concrete over vague — put the real numbers in

A spec that says "includes a margin", "within a reasonable time", or "a
suitable threshold" has not specified anything. State the formula, the
constant, the threshold. *This* spec is the place the number is decided —
don't defer it to the implementation.

### 8. Presentation — tables, math, and figures

Markdown tables, LaTeX math, and figures are all fair game; reach for them
when they read better than prose. They may be **inline** in the "shall" or
acceptance, or placed once in the doc and **referenced** by several specs
(e.g. a shared constants table). Keep formulas on their own line with
surrounding blank lines so they render and scan cleanly.

**Strongly prefer a table when a spec lists two or more equivalent-level
options and their resulting detail.** Parallel options in prose become a
run-on block that's hard to scan and easy to misread; a table makes the
options a column and their behavior a column, so the structure is visible
at a glance and a missing or asymmetric case is obvious.

> ✗ "...for GPIO mode it shall assert the configured CS GPIO ...; for
>    hardware mode it shall rely on native NSS; for none it shall drive no
>    chip-select." (three parallel modes run into one block)
> ✓ a `| CS mode | Driver behavior |` table with one row per mode.

### 9. Every detail must be relevant — and at the right altitude

Include only detail that bears on the one behavior being specified, pitched
at the altitude of the text around it. Two failure modes:

- **Wrong concern.** A fact can be true and still not belong — if it
  belongs to a different concern, it goes in that concern's spec (or
  nowhere). A timeout spec does not discuss byte ordering.
- **Wrong altitude.** Detail that needs a "because…" to justify itself is a
  signal it deserves its own spec, not an inline aside in a higher-level
  description. When a structural overview starts explaining *why* a
  property holds, extract that into a dedicated spec and let the overview
  just name the thing.

> ✗ (in a one-line config overview) "Each bus declares its transfer mode;
>    transfer mode is a property of the physical peripheral and its DMA
>    wiring, so it lives on the bus."
> ✓ (overview) "Buses enumerate the physical SPI peripherals." + a separate
>    spec that defines transfer modes.

### 10. Acceptance bullets: testable, non-redundant, single-concern

The `Acceptance:` block exists to make the one requirement verifiable.
Every bullet earns its place against three tests:

- **Testable.** A bullet a SIL scenario or unit test could actually check.
  A bullet that can't be tested — or would require an API the spec doesn't
  define — is describing nothing, or smuggling a *different* requirement
  that deserves its own spec. Cut it either way.
- **Non-redundant.** If a bullet only restates the "shall", cut it. If it
  carries a detail the "shall" omits, move that detail *into* the shall and
  then cut the bullet.
- **Single-concern.** A bullet verifies *this* spec's behavior, not a
  neighbor's (see rule 9).

> ✗ "A bus's transfer mode is fixed for the life of the configuration."
>    (no API makes it changeable — untestable, and really a different
>    requirement about config immutability)

### 11. Unambiguous in the context of its doc

The test for whether a spec carries enough qualification is not a fixed
rule about flat vs. hierarchical organization — it is a question:

> Could a reasonable human or agent, reading this spec **within the
> document it lives in**, understand its context and not misinterpret it?

Both styles satisfy this:

- A **flat** doc where each spec carries an inline qualifier — "On a bus
  configured for software transfer mode, the driver shall…" — to scope when
  the behavior applies.
- A **hierarchical** doc that groups specs under Markdown `##`/`###`
  headings (e.g. a "Transfer modes" section), where the section supplies
  the context and the individual "shall" needs no inline qualifier.

Use whichever keeps the spec readable and unambiguous. Don't add inline
qualification that the surrounding structure already makes obvious, and
don't rely on structure that a reader could miss.

These organizing tools are all legitimate — reach for them when they aid
readability:

- **Section headings** to group related specs (specs stay at `###`; group
  headers go at `##` so spec headings don't get pushed deeper). The header
  name carries the grouping; **do not add explanatory subtext under it**
  that just re-narrates the specs below — the header and the spec titles
  are descriptive enough. (Subtext is justified only to state a grouping
  *rationale* the header can't, which is rare.)
- **Cross-references to other specs by ID**, in acceptance *or* prose
  (e.g. "performs transfers per `fw~hal_spi_003~1`").
- **A foundational "what" spec** that lays out a taxonomy and organizes the
  specs that follow it (e.g. a transfer-modes spec above the per-mode
  specs). It earns its keep when the *what* is genuinely foundational and
  aids navigation, even if its own testable claim is a thin smoke test.

### 12. Stateless, end-state language

A spec describes the **finished** system, not the path to it. Never write
words that encode the implementation timeline:

> ✗ now · currently · until then · not yet · will eventually · for the
> time being · in a future version · once X is implemented

The spec is the north star — its wording does not change as intermediate
build steps land. If a behavior isn't built yet, the spec still states it
in the present tense as a standing requirement.

### 13. No reference to past implementations

Source control is the record of history. A spec describes the code as it
should be, not what it used to be. No "changed from", "previously",
"replaces the old".

### 14. Mechanics that affect wording

(Full detail in `spec-system.md`; repeated here because they're easy to get
wrong while writing.)

- The `` `<id>~1` `` line sits **directly under** its `###` heading, no
  blank line between.
- Language-hint every code fence (` ```c `, ` ```text `); a bare fence
  breaks OFT's parser for the rest of the file.
- No `---` thematic breaks in spec body text — they read as frontmatter
  delimiters. Use a heading instead.

## Spec anatomy and the optional `Rationale:` section

The required parts of a spec are defined in
[`spec-system.md`](spec-system.md#spec-template): heading, ID line, the
"shall" paragraph, `Acceptance:`, `Covers:`, `Needs:`. One **optional**
block may appear:

- **`Rationale:`** — placed after the "shall" paragraph and before
  `Acceptance:`. Permitted **only on a subjective spec** where several
  viable alternatives existed and the judgement behind the chosen one is
  worth preserving. It is the sanctioned home for the *why* that rule 5
  keeps out of the requirement prose — and an escape hatch, not a default
  part of a spec. The vast majority of specs omit it.

```markdown
### Some behavior
`fw~topic_001~1`

The driver shall <single behavior>.

Rationale:
- <why this choice over the viable alternatives — context, not requirement>

Acceptance:
- <testable bullet>

Covers:
- sys~...

Needs: impl, test
```

Keep `Rationale:` rare and short. Omitting it is the norm — most specs are
self-evidently why they are, and need no justification. Reach for it only
when leaving the rationale out would lose a non-obvious judgement a future
reader couldn't reconstruct. It is never a substitute for a clear "shall",
and OFT ignores it (it carries no trace).

## Quick checklist

Run before declaring a spec done:

- [ ] Exactly one "shall" (rule 1).
- [ ] No word that could be cut without changing meaning (rule 2).
- [ ] States the chosen behavior only — no "rather than / not Y" (rule 3).
- [ ] Affirmative — says what the system is, not what it isn't or what
      another layer does (rule 4).
- [ ] No rationale/"so that…" in the requirement; any why lives in an
      optional `Rationale:` section (rule 5).
- [ ] No constraint the requirement doesn't actually need (rule 6).
- [ ] Concrete numbers/formulas, not vague margins or thresholds (rule 7).
- [ ] Table used for 2+ parallel options and their detail, not a prose
      run-on (rule 8).
- [ ] Every detail relevant and at the right altitude — detail needing a
      "because…" gets its own spec (rule 9).
- [ ] Acceptance bullets testable, non-redundant, single-concern (rule 10).
- [ ] Unambiguous in the context of its doc (rule 11).
- [ ] No timeline/temporal words (rule 12).
- [ ] No reference to past implementations (rule 13).
- [ ] ID line directly under heading; fences language-hinted; no `---`
      (rule 14).
- [ ] `tools/validate-specs.py` passes.
