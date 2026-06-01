---
name: pcs_spec
description: Author OpenFastTrace specs for the pcs_bldc project (spec-driven development with end-to-end traceability). Use this whenever writing, revising, or planning a `sys~`, `fw~`, or `app~` requirement spec — adding a new behavior to the spec tree, back-filling specs for existing code (e.g. HW_ADC), or deciding where a new requirement fits. Drives an interview-then-research workflow: it interviews the user for true intent, then fans out two research agents (one over the existing spec tree, one over the codebase) before co-authoring compliant spec language. Trigger on "write a spec", "spec out this driver", "add a fw~ spec", "back-fill specs for X", or any spec-authoring task in this repo.
---

# pcs_bldc spec authoring

This skill is the procedure for adding requirement specs to the `pcs_bldc`
spec tree. It exists because good specs here are not a transcription task —
they require pulling the user's true intent out of their head, placing the
new requirement correctly relative to what already exists, and (for `fw~`/
`app~` specs) grounding the language in how the code is actually built.

## Sources of truth — read these first

Before doing anything, read (or confirm you've already read this session):

1. `docs/spec-system.md` — **the** authority on mechanics. Artifact types,
   the coverage rule, ID convention, file organization, anti-bloat rules,
   OFT gotchas.
2. `docs/spec-style.md` — **the** authority on wording. Every rule for how
   the "shall" and acceptance bullets are phrased lives there: conciseness,
   affirmative framing, not over-constraining, concrete numbers, relevant
   detail at the right altitude, testable/non-redundant acceptance,
   stateless language. Read it in full before drafting any spec language.
3. `docs/spec-template.md` — worked examples of every spec shape (`fw~`,
   `app~`, cross-component `sys~`) plus the code/test tag syntax.
4. `specs/system/overview.md` — the architectural picture and the anchor
   `sys~` specs every other spec ultimately traces to.
5. The relevant component README: `specs/firmware/README.md` or
   `specs/desktop-app/README.md` for topic folders and tag conventions.

For `fw~` specs that imply C code, also read
`docs/c-coding-conventions.md` so any implementation guidance you produce
is house-style from the start.

Do not summarize these back to the user — just absorb them so the spec you
write is compliant without a second pass.

## The rules live in the docs — do not restate them here

All **wording** rules are in `docs/spec-style.md`; all **mechanics** (trace,
IDs, file layout, OFT) are in `docs/spec-system.md`. This skill deliberately
does **not** copy them — a second copy drifts. Read both before drafting,
hold them in mind through Phase 4, and run `spec-style.md`'s checklist plus
`tools/validate-specs.py` before declaring a spec done.

The two highest-leverage habits, as orientation only (the docs are
authoritative):

- **One spec, one shall** — the single requirement, stated affirmatively,
  concisely, with concrete numbers and no detail belonging to another spec.
- **Every spec traces up** — `Covers:` a real parent (`sys~` for
  `fw~`/`app~`; a named README goal for top `sys~`), or it fails OFT.

## ID convention quick reference

`<type>~<topic>[_<subtopic>]_<NNN>~1`

- `type` ∈ `{sys, fw, app}`.
- `topic` from the canonical table in `docs/spec-system.md`. Adding a new
  topic = add a row there first, then number from `_001`.
- `subtopic` (optional) gives a busy topic per-area number spaces. The
  `hal` topic uses one per peripheral: `hal_spi`, `hal_adc`, `hal_gpio`,
  `hal_dma`, `hal_tim`. Most topics omit it.
- `NNN` sequential within the `(type, topic, subtopic)` tuple.

Run `tools/validate-specs.py` after writing to confirm compliance.

## Workflow

The skill runs in six phases. Do not skip the first interview, the research
fan-out, or the fresh-context style audit — they are the point. The second
interview is conditional; the rest are not.

### Phase 1 — Interview for true intent

Specs fail when they encode the assistant's assumptions instead of the
user's intent. Before writing anything, interview the user. Ask focused,
decision-shaped questions (use the AskUserQuestion tool for the crisp
either/or ones; ask open questions in prose when you need a paragraph of
context). Cover, as relevant:

- **The behavior's purpose** — what real capability does this enable? What
  breaks or is impossible without it? This becomes the `Acceptance:`
  intent and informs which `sys~` parent it traces to.
- **Scope boundary** — what is explicitly *out*? (Guards against the
  "one spec, one shall" violation.)
- **Consumers / dependents** — who uses this, at which layer? (`app→dev→
  io→hw`.) Shapes the API the spec implies.
- **Success/failure criteria** — what does "working" look like concretely
  and measurably enough to write testable acceptance bullets?
- **New-build vs back-fill** — is this a behavior to be built, or are we
  writing specs *after the fact* for code that already exists (the HW_ADC
  case)? This changes how Phase 2 runs.

Keep interviewing until you can state the one "shall" in a single sentence.
If you can't, you don't understand it yet — ask more.

### Phase 2 — Fan out two research agents (in parallel)

Spawn both agents in a single message so they run concurrently. Give each
the behavior description distilled from the interview. Each returns a
written briefing, not file dumps.

**Agent A — spec-tree cartographer** (`Explore` subagent type is a good
fit). Task it to:
- Map where this behavior fits in the existing spec tree.
- Find **overlap and commonality** with existing `sys~`/`fw~`/`app~`
  specs — is part of this already covered? Would it duplicate something?
- Identify the correct **`sys~` parent** to trace up to (or flag that a
  new `sys~` anchor is needed, and where it should live).
- Recommend **placement**: which topic/subtopic, which file, whether it
  joins an existing multi-spec file or starts a new one.
- Surface the **next available ID** for the likely (type, topic, subtopic).
- Flag any anti-bloat concern (is this spec actually earning its keep, or
  is it an LLR that a parent spec + clear code already covers?).

**Agent B — codebase investigator** (`Explore` or `general-purpose`).
Only essential for `fw~`/`app~` specs. Task it to:
- Study how **adjacent systems are implemented** so any implementation
  direction in the spec matches established patterns (channelization,
  init-returns-bool, the layered split, naming).
- **If the feature already exists** (back-fill case): understand the
  current implementation in detail, then assess whether it *diverges* from
  what the intended spec language requires. Concretely list:
  - What the spec would mandate vs what the code currently does.
  - Changes the existing implementation needs to become spec-compliant.
  - Any place the spec should bend to a justified existing decision rather
    than forcing churn.
- Return concrete `file:line` references and the implications for spec
  wording — not a code tour.

Read both briefings fully before moving on. They will change where the
spec lives, what it traces to, and how its acceptance bullets are phrased.

### Phase 3 — Second interview (only if open questions remain)

The research routinely surfaces things the first interview couldn't have
anticipated: an overlap with an existing spec that forces a scope decision,
a missing `sys~` parent that needs the user's call on whether to author a
new anchor, or — especially in the back-fill case — a divergence between
what the user described and what the code actually does.

Collect everything Phase 2 left unresolved and bring it back to the user as
a focused second round. Frame each question with the context that raised it
("Agent B found the current HW_ADC code does X, but your intent implies Y —
which wins?"). Use AskUserQuestion for the crisp decisions.

**If the research left no open questions, skip this phase** — do not invent
questions to fill it. Going straight to drafting is correct when intent and
research already agree.

The bar for moving on: there is no unresolved decision that would change the
spec's wording, its parent, or its placement.

### Phase 4 — Draft the spec(s)

Now that intent and research agree, draft the spec(s) strictly per
`docs/spec-style.md`. This is a working draft, held in the chat — not yet
written to disk and not yet shown to the user for sign-off. For each spec
produce:

- An `### H3` heading (matches the file `# H1` for a one-spec `sys~` file).
- The `` `<id>~1` `` line directly beneath it.
- One "shall" paragraph — stateless, single behavior.
- A testable `Acceptance:` block — bullets a SIL or unit test could check.
- `Covers:` the parent (from Agent A).
- `Needs:` the right downstream set (`impl, test` for `fw~`/`app~`;
  `fw`/`app`/`test` subset for `sys~`).

### Phase 5 — Fresh-context style audit

The author of a draft cannot see it cleanly — the interview and research
rationale sitting in your context makes it easy to read past wording the
rules forbid. So hand the raw draft to a reviewer who has none of that
context and only the rules.

Spawn a **single-purpose auditor subagent** (`Explore` is a good fit).
Critically, **give it no conversation context** — not the interview, not
the research briefings, not the rationale. Its entire input is:

- The literal text of the drafted spec(s).
- An instruction to read **both** `docs/spec-style.md` (the wording rules)
  and `docs/spec-system.md` (the mechanics, and the conventions it
  *endorses* — e.g. MOC files and `[[wiki-links]]` for navigation), and to
  check the draft against every rule and the end-of-doc style checklist.
  Without the mechanics doc the auditor false-positives on endorsed
  conventions.
- A request to return, per finding: the rule number, the offending text
  quoted verbatim, and why it violates — and nothing about specs that are
  clean. No rewrite suggestions; judgment of compliance only.

Its blindness is the feature: if a spec only makes sense with the
conversation in hand, that's a context-ambiguity finding worth having.

Take the auditor's findings back into the draft and resolve each — either
fix the wording or, if you judge the auditor wrong (it lacks context by
design and can misread), note why it's a false positive. Re-audit with a
fresh auditor if the changes were substantial. Only a clean (or
consciously-dispositioned) audit proceeds to Phase 6.

### Phase 6 — Review and commit

Present the audited draft **in the chat** for the user to review and react
to. Iterate with the user until they approve it. Only once they sign off,
write the spec(s) into the spec directory at the placement Agent A
recommended, then:

- Add/confirm the topic row in `docs/spec-system.md` if a new topic.
- Update the component README topic list if a new subfolder.
- Run `tools/validate-specs.py` and report the result.
- For back-fill specs where Agent B found divergence: present the required
  implementation changes as a clear follow-up list. Tag the code with
  `// [impl->...]` / `// [test->...]` only when the user is ready — don't
  silently retrofit tags onto code that doesn't yet satisfy the spec.
