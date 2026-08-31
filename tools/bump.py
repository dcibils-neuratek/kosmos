#!/usr/bin/env python3
"""Moves the version in VERSION.

major.minor.revision, and which one moves is a decision rather than a
calculation:

    revision   every push
    minor      every milestone
    major      when something was big enough that we say so

Bumping a level zeroes the ones below it, which is the only part of this
worth writing code for - it is also the part people get wrong by hand.
"""

import pathlib
import sys

FILE = pathlib.Path(__file__).resolve().parent.parent / "VERSION"


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "revision"

    if which not in ("major", "minor", "revision"):
        raise SystemExit("bump.py [major|minor|revision]")

    parts = FILE.read_text().strip().split(".")

    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        raise SystemExit(f"VERSION does not hold major.minor.revision: {parts}")

    major, minor, revision = (int(p) for p in parts)

    if which == "major":
        major, minor, revision = major + 1, 0, 0
    elif which == "minor":
        minor, revision = minor + 1, 0
    else:
        revision += 1

    new = f"{major}.{minor}.{revision}"
    FILE.write_text(new + "\n")
    print(new)


if __name__ == "__main__":
    main()
