#!/usr/bin/env python3
"""Describe every file and source used by a built portable runtime."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime", type=Path)
    parser.add_argument("source_lock", type=Path)
    parser.add_argument("packages", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    lock = json.loads(args.source_lock.read_text(encoding="utf-8"))
    package_lines = sorted(
        line for line in args.packages.read_text(encoding="utf-8").splitlines() if line
    )
    files = []
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file() and path != args.output:
            files.append(
                {
                    "path": path.relative_to(args.runtime).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": sha256(path),
                }
            )
    manifest = {
        "schema": 1,
        "name": lock["name"],
        "sources": {
            name: {
                "repository": lock[name]["repository"],
                "commit": lock[name]["commit"],
                "baseTag": lock[name]["baseTag"],
                "baseCommit": lock[name]["baseCommit"],
                "license": lock[name]["license"],
            }
            for name in ("qemu", "virglrenderer")
        },
        "msys2Packages": package_lines,
        "files": files,
    }
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
