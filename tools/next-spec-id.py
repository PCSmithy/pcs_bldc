#!/usr/bin/env python3
"""Find the next available OFT spec number for a given (type, topic) tuple.

Scans the specs/ tree for existing IDs of the form <type>~<topic>_NNN~* and
returns the next sequential number, zero-padded to 3 digits, in the project's
canonical ID format. Validates the topic against the canonical table in
docs/spec-system.md (parsed via spec_convention.py) so typos surface
immediately.

Usage:
  tools/next-spec-id.py <type> <topic> [subtopic]
  tools/next-spec-id.py --list <type> <topic> [subtopic]

Examples:
  tools/next-spec-id.py sys mc        # -> sys~mc_001~1
  tools/next-spec-id.py fw mc         # -> fw~mc_007~1 (if 001-006 are taken)
  tools/next-spec-id.py fw hal spi    # -> fw~hal_spi_001~1 (own number space)
  tools/next-spec-id.py --list sys arch
"""

import argparse
import sys

import spec_convention as sc


def main():
    parser = argparse.ArgumentParser(
        description="Find the next available spec number for a (type, topic) tuple."
    )
    parser.add_argument(
        "--list",
        action="store_true",
        dest="show_list",
        help="List all existing spec IDs in this (type, topic) before showing the next.",
    )
    parser.add_argument("type", choices=sc.VALID_TYPES, help="sys / fw / app")
    parser.add_argument(
        "topic", help="Topic abbreviation (see docs/spec-system.md)"
    )
    parser.add_argument(
        "subtopic", nargs="?", default=None,
        help="Optional sub-topic for its own number space (e.g. 'spi' under "
             "'hal' -> fw~hal_spi_NNN~1).",
    )
    args = parser.parse_args()

    valid_topics = sc.parse_topic_table()
    if not valid_topics:
        print(
            f"Error: could not parse topic table from {sc.relpath(sc.SPEC_SYSTEM_MD)}.",
            file=sys.stderr,
        )
        sys.exit(2)
    if args.topic not in valid_topics:
        print(
            f"Error: '{args.topic}' is not in the canonical topic table.\n"
            f"Valid topics: {', '.join(sorted(valid_topics))}\n"
            f"To add a new topic, edit {sc.relpath(sc.SPEC_SYSTEM_MD)} first.",
            file=sys.stderr,
        )
        sys.exit(2)

    definitions, _ = sc.find_spec_id_lines()
    existing = sorted(
        int(d.number) for d in definitions
        if d.type == args.type
        and d.topic == args.topic
        and d.subtopic == args.subtopic
    )
    next_n = (existing[-1] + 1) if existing else 1

    base = f"{args.type}~{args.topic}"
    if args.subtopic:
        base = f"{base}_{args.subtopic}"
    next_id = f"{base}_{next_n:03d}~1"

    if args.show_list:
        if existing:
            print(f"Existing {base}_NNN~1 specs:")
            for n in existing:
                print(f"  {base}_{n:03d}~1")
        else:
            print(f"No existing {base}_NNN~1 specs.")
        print()
        print(f"Next: {next_id}")
    else:
        print(next_id)


if __name__ == "__main__":
    main()
