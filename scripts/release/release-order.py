#!/usr/bin/env python3
"""Decide whether a release should replace the current Latest download."""

import re
import sys

PATTERN = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-preview)?")


def parse(tag):
    match = PATTERN.fullmatch(tag)
    if not match:
        raise ValueError(f"invalid release tag: {tag}")
    return (*map(int, match.group(1, 2, 3)), match.group(4) is None)


def should_promote(candidate, current):
    new, old = parse(candidate), parse(current)
    return new > old and not (old[3] and not new[3])


if __name__ == "__main__":
    try:
        print(str(should_promote(*sys.argv[1:])).lower())
    except (ValueError, TypeError) as error:
        raise SystemExit(str(error))
