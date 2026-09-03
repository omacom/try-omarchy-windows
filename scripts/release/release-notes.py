#!/usr/bin/env python3
"""Extract one release section from CHANGELOG.md."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("tag")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--notes-dir", type=Path, default=Path(".github/release-notes"))
    args = parser.parse_args()

    if Path(args.tag).name != args.tag:
        raise SystemExit("tag must not contain a path")
    notes = args.notes_dir / f"{args.tag}.md"
    if notes.is_file():
        body = notes.read_text(encoding="utf-8").strip()
        if not body:
            raise SystemExit(f"{notes} is empty")
        print(body)
        return

    lines = args.changelog.read_text(encoding="utf-8").splitlines()
    header = re.compile(rf"^## {re.escape(args.tag)}(?:\s+-\s+.+)?$")
    start = next((index + 1 for index, line in enumerate(lines) if header.fullmatch(line)), None)
    if start is None:
        raise SystemExit(f"CHANGELOG.md has no section for {args.tag}")
    end = next((index for index in range(start, len(lines)) if lines[index].startswith("## ")), len(lines))
    body = "\n".join(lines[start:end]).strip()
    if not body:
        raise SystemExit(f"CHANGELOG.md section for {args.tag} is empty")
    print(body)


if __name__ == "__main__":
    main()
