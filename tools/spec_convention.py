"""Shared helpers for spec convention parsing.

Single source of truth for the canonical topic abbreviations is the table in
`docs/spec-system.md` under the heading "### Topic abbreviations". This
module parses that table at runtime so the convention can never drift between
the documentation and the tooling.

Used by:
  - tools/next-spec-id.py   (find next available number)
  - tools/validate-specs.py (verify all spec IDs match the convention)
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPECS_DIR = REPO_ROOT / "specs"
SPEC_SYSTEM_MD = REPO_ROOT / "docs" / "spec-system.md"

VALID_TYPES = ("sys", "fw", "app")
REQUIRED_VERSION = "1"  # project policy: always ~1

# Strict spec ID format on its own line: `<type>~<topic>_<NNN>~<version>`
STRICT_ID_LINE = re.compile(
    r"^`([a-z]+)~([a-z]+)_(\d+)~(\d+)`\s*$"
)
# Loose: any backticked thing on its own line that has the shape word~word~word.
# Used to flag near-misses (typos, wrong format) as violations rather than
# silently ignoring them.
LOOSE_ID_LINE = re.compile(
    r"^`([^`\s]+~[^`\s]+~[^`\s]+)`\s*$"
)


@dataclass
class SpecDef:
    file: Path
    line: int
    raw_id: str
    type: str
    topic: str
    number: str   # zero-padded, e.g. "001"
    version: str


@dataclass
class Violation:
    file: Path
    line: int
    raw_id: str
    message: str


def parse_topic_table(path: Path = SPEC_SYSTEM_MD) -> set[str]:
    """Return the set of canonical topic abbreviations from spec-system.md.

    Parses the markdown table under "### Topic abbreviations". Each row's
    first cell holds the abbreviation in backticks: `| \\`mc\\` | ... |`.
    """
    text = path.read_text(encoding="utf-8")
    abbrevs: set[str] = set()
    in_section = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "### Topic abbreviations":
            in_section = True
            continue
        if in_section:
            if stripped.startswith("##"):  # next H2/H3 ends the section
                break
            m = re.match(r"\|\s*`(\w+)`\s*\|", line)
            if m:
                abbrevs.add(m.group(1))
    return abbrevs


def find_spec_id_lines(
    specs_dir: Path = SPECS_DIR,
) -> tuple[list[SpecDef], list[Violation]]:
    """Walk specs/, parsing every line that looks like a spec ID definition.

    Returns (definitions, violations). A line is treated as a spec ID line if
    it is a single backticked token containing two tilde separators. Lines
    that don't match that loose shape are ignored entirely (they're narrative
    or `Covers:` references). Lines that match the loose shape but fail the
    strict format become violations.
    """
    definitions: list[SpecDef] = []
    violations: list[Violation] = []
    if not specs_dir.exists():
        return definitions, violations

    for md_file in sorted(specs_dir.rglob("*.md")):
        try:
            text = md_file.read_text(encoding="utf-8")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            loose = LOOSE_ID_LINE.match(line)
            if not loose:
                continue
            strict = STRICT_ID_LINE.match(line)
            if strict:
                t, topic, number, version = strict.groups()
                definitions.append(SpecDef(
                    file=md_file, line=lineno, raw_id=loose.group(1),
                    type=t, topic=topic, number=number, version=version,
                ))
            else:
                violations.append(Violation(
                    file=md_file, line=lineno, raw_id=loose.group(1),
                    message="does not match `<type>~<topic>_<NNN>~<version>` format",
                ))
    return definitions, violations


def relpath(path: Path) -> str:
    """Return path relative to repo root, with forward slashes for portability."""
    try:
        return str(path.relative_to(REPO_ROOT)).replace("\\", "/")
    except ValueError:
        return str(path)
