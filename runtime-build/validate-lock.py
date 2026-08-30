#!/usr/bin/env python3
"""Validate the immutable source recipe for the Windows QEMU runtime."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from urllib.parse import urlparse


SHA = re.compile(r"^[0-9a-f]{40}$")
COMPONENTS = {"qemu", "virglrenderer"}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "lock", type=Path, nargs="?", default=Path(__file__).with_name("sources.lock.json")
    )
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text(encoding="utf-8"))

    if lock.get("schema") != 1 or not isinstance(lock.get("sourceDateEpoch"), int):
        raise SystemExit("runtime source lock has an unsupported schema")
    if set(lock) != {"schema", "name", "sourceDateEpoch", *COMPONENTS}:
        raise SystemExit("runtime source lock has unexpected fields")
    if not re.fullmatch(r"winq-emu-[a-z0-9-]+", lock.get("name", "")):
        raise SystemExit("runtime source lock has an invalid name")

    for name in sorted(COMPONENTS):
        component = lock[name]
        expected = {"repository", "commit", "upstream", "baseTag", "baseCommit", "license"}
        if set(component) != expected:
            raise SystemExit(f"{name} source lock has unexpected fields")
        for field in ("commit", "baseCommit"):
            if not SHA.fullmatch(component.get(field, "")):
                raise SystemExit(f"{name} {field} is not a full Git commit")
        for field in ("repository", "upstream"):
            parsed = urlparse(component.get(field, ""))
            if parsed.scheme != "https" or parsed.hostname not in {
                "github.com",
                "gitlab.freedesktop.org",
            }:
                raise SystemExit(f"{name} {field} is not an approved HTTPS source")
        if component["commit"] == component["baseCommit"]:
            raise SystemExit(f"{name} fork commit does not contain a patch series")

    print(
        "ok - locked QEMU "
        f"{lock['qemu']['commit'][:12]} and virglrenderer "
        f"{lock['virglrenderer']['commit'][:12]}"
    )


if __name__ == "__main__":
    main()
