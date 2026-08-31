#!/usr/bin/env python3
"""Validate that all spec IDs in specs/ follow the project convention.

Convention (defined in docs/spec-system.md):
  <type>~<topic>_<NNN>~<version>

  type     in (sys, fw, app)
  topic    in the canonical table parsed from spec-system.md
  NNN      exactly 3 digits, zero-padded
  version  always 1 (project policy)

Also enforces uniqueness: the same ID may not be defined in two places.

Exit codes:
  0  all spec IDs valid
  1  one or more violations found
  2  internal error (e.g. could not parse topic table)

Usage:
  tools/validate-specs.py            # validate, print summary, exit 0/1
  tools/validate-specs.py --quiet    # only print on failure
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

import spec_convention as sc


def format_violation(v: sc.Violation) -> str:
    return (
        f"  {sc.relpath(v.file)}:{v.line}\n"
        f"    `{v.raw_id}`\n"
        f"    {v.message}"
    )


def validate_definitions(
    definitions: list[sc.SpecDef], valid_topics: set[str]
) -> list[sc.Violation]:
    """Apply convention rules to each parsed spec definition."""
    violations: list[sc.Violation] = []
    for d in definitions:
        if d.type not in sc.VALID_TYPES:
            violations.append(sc.Violation(
                file=d.file, line=d.line, raw_id=d.raw_id,
                message=(
                    f"invalid type '{d.type}' "
                    f"(must be one of: {', '.join(sc.VALID_TYPES)})"
                ),
            ))
            continue  # don't double-report on a bogus type
        if d.topic not in valid_topics:
            violations.append(sc.Violation(
                file=d.file, line=d.line, raw_id=d.raw_id,
                message=(
                    f"invalid topic '{d.topic}' (not in canonical table); "
                    f"valid topics: {', '.join(sorted(valid_topics))}"
                ),
            ))
            continue
        if len(d.number) != 3:
            violations.append(sc.Violation(
                file=d.file, line=d.line, raw_id=d.raw_id,
                message=(
                    f"number '{d.number}' must be exactly 3 digits, "
                    f"zero-padded (e.g. '001', '042')"
                ),
            ))
        if d.version != sc.REQUIRED_VERSION:
            violations.append(sc.Violation(
                file=d.file, line=d.line, raw_id=d.raw_id,
                message=(
                    f"version '~{d.version}' must be '~{sc.REQUIRED_VERSION}' "
                    f"by project policy (specs are edited in place, not "
                    f"version-bumped)"
                ),
            ))
    return violations


def find_duplicates(definitions: list[sc.SpecDef]) -> list[sc.Violation]:
    """A given ID may only be defined once across all of specs/."""
    by_id: dict[str, list[sc.SpecDef]] = defaultdict(list)
    for d in definitions:
        by_id[d.raw_id].append(d)
    violations: list[sc.Violation] = []
    for raw_id, defs in by_id.items():
        if len(defs) <= 1:
            continue
        first = defs[0]
        for d in defs[1:]:
            violations.append(sc.Violation(
                file=d.file, line=d.line, raw_id=raw_id,
                message=(
                    f"duplicate ID; first defined at "
                    f"{sc.relpath(first.file)}:{first.line}"
                ),
            ))
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate spec IDs in specs/ against the project convention."
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="Only print on failure; print nothing on success.",
    )
    args = parser.parse_args()

    valid_topics = sc.parse_topic_table()
    if not valid_topics:
        print(
            f"Error: could not parse topic table from {sc.relpath(sc.SPEC_SYSTEM_MD)}.",
            file=sys.stderr,
        )
        return 2

    definitions, format_violations = sc.find_spec_id_lines()
    convention_violations = validate_definitions(definitions, valid_topics)
    duplicate_violations = find_duplicates(definitions)
    all_violations = format_violations + convention_violations + duplicate_violations

    files_scanned = len({d.file for d in definitions} |
                        {v.file for v in format_violations})

    if not all_violations:
        if not args.quiet:
            print(
                f"Validated {len(definitions)} spec definitions across "
                f"{files_scanned} file(s). All conform to convention."
            )
        return 0

    print(
        f"Found {len(definitions)} spec definitions across "
        f"{files_scanned} file(s).",
        file=sys.stderr,
    )
    print(file=sys.stderr)
    print(f"{len(all_violations)} violation(s):", file=sys.stderr)
    print(file=sys.stderr)
    for v in all_violations:
        print(format_violation(v), file=sys.stderr)
        print(file=sys.stderr)
    print("Validation failed.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
