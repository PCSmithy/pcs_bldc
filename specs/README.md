# pcs_bldc — specs

This is the Map-of-Content (MOC) for the project's spec system. See
[`../docs/spec-system.md`](../docs/spec-system.md) for the formal definition
(artifact types, coverage rules, OFT integration, anti-bloat rules).

The `specs/` tree is designed to read well both as plain Markdown on GitHub
and as an Obsidian vault: each `README.md` is a folder MOC, individual specs
live in atomic Markdown files, and `[[wiki-links]]` cross-reference between
them for human navigation.

## Areas

- [[system/README|System]] — `sys~` requirements: what the system as a
  whole must do, including how firmware and the desktop app cooperate.
- [[firmware/README|Firmware]] — `fw~` requirements: STM32G4 firmware
  behavior, in C/C++ on FreeRTOS.
- [[desktop-app/README|Desktop app]] — `app~` requirements: Rust GUI for
  configuration / operation / diagnostics.

## Conventions in brief

- YAML frontmatter on every spec file (`status`, `tags`).
- Each spec is `### Heading` followed by `` `type~name~version` `` — see
  [`../docs/spec-template.md`](../docs/spec-template.md) for worked examples
  of `fw~`, `app~`, and cross-component `sys~` specs.
- Formal trace via `Covers:` and `Needs:` blocks (OFT-readable).
- Narrative cross-references via `[[wiki-links]]` (Obsidian-readable).
- One spec per file for `sys~`; aggregate per file for `fw~` / `app~` when
  tightly related.

The template lives outside `specs/` because OFT scans `specs/` for real
specs and would otherwise pick up the example IDs from the template's code
blocks as uncovered.

## Adding a spec

1. Pick the right component folder (`system/`, `firmware/`, or `desktop-app/`).
2. Place under an existing topic sub-folder, or create a new one if there
   isn't a fit. (Do not pre-create empty topic folders.)
3. Copy the structure from [`../docs/spec-template.md`](../docs/spec-template.md).
4. Verify with `tools/oft/oft.sh trace specs/` — expect at least the new
   spec to appear in the trace output, and to be flagged uncovered until
   you add the corresponding `impl` and `test` tags in code.
