#!/usr/bin/env python3
"""Validate the immutable source recipe for the Windows QEMU runtime."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from urllib.parse import urlparse


SHA = re.compile(r"^[0-9a-f]{40}$")
COMPONENTS = {"qemu", "virglrenderer"}


def validate_patches(name: str, patches: object, recipe: Path) -> None:
    """Patches apply on top of the fork commit, so each is pinned by digest
    and must live under the recipe's patches directory."""
    if not isinstance(patches, list):
        raise SystemExit(f"{name} patches must be a list")
    seen = set()
    for entry in patches:
        if not isinstance(entry, dict) or set(entry) != {"file", "sha256"}:
            raise SystemExit(f"{name} patch entries need exactly file and sha256")
        relative = entry["file"]
        if not re.fullmatch(rf"patches/{name}/[0-9]{{4}}-[A-Za-z0-9._-]+\.patch", relative):
            raise SystemExit(f"{name} patch has an invalid path: {relative}")
        if relative in seen:
            raise SystemExit(f"{name} patch is listed twice: {relative}")
        seen.add(relative)
        path = recipe / relative
        if not path.is_file():
            raise SystemExit(f"{name} patch is missing: {relative}")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != entry["sha256"]:
            raise SystemExit(f"{name} patch digest mismatch: {relative}")


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
        if set(component) - {"patches"} != expected:
            raise SystemExit(f"{name} source lock has unexpected fields")
        validate_patches(name, component.get("patches", []), args.lock.parent)
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

    patch_count = sum(len(lock[name].get("patches", [])) for name in COMPONENTS)
    print(
        "ok - locked QEMU "
        f"{lock['qemu']['commit'][:12]} and virglrenderer "
        f"{lock['virglrenderer']['commit'][:12]} with {patch_count} recipe patch(es)"
    )


if __name__ == "__main__":
    main()
