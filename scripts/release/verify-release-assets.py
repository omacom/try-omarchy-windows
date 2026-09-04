#!/usr/bin/env python3
"""Verify selected release assets against an authenticated SHA256SUMS file."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


LINE_RE = re.compile(r"([0-9a-f]{64}) [ *]([^/\\\s]+)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("directory", type=Path)
    parser.add_argument("names", nargs="+")
    args = parser.parse_args()

    entries: dict[str, str] = {}
    for number, line in enumerate(
        args.manifest.read_text(encoding="utf-8").splitlines(), 1
    ):
        match = LINE_RE.fullmatch(line)
        if not match:
            raise SystemExit(f"invalid SHA256SUMS line {number}")
        digest, name = match.groups()
        if name in entries:
            raise SystemExit(f"duplicate SHA256SUMS entry: {name}")
        entries[name] = digest

    for name in args.names:
        if Path(name).name != name or name not in entries:
            raise SystemExit(f"manifest has no safe entry for {name}")
        path = args.directory / name
        if path.is_symlink() or not path.is_file():
            raise SystemExit(f"release asset is missing or unsafe: {name}")
        if sha256(path) != entries[name]:
            raise SystemExit(f"release asset SHA256 mismatch: {name}")
        print(f"ok - {name}")


if __name__ == "__main__":
    main()
