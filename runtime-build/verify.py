#!/usr/bin/env python3
"""Verify the runtime output before GitHub accepts it as an artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path, PurePosixPath


REQUIRED = {
    "bin/qemu-system-x86_64.exe",
    "bin/qemu-system-x86_64w.exe",
    "bin/qemu-img.exe",
    "bin/libvirglrenderer-1.dll",
    "bin/SDL2.dll",
    "bin/share/bios-256k.bin",
    "provenance/sources.lock.json",
    "provenance/runtime-manifest.json",
    "provenance/msys2-packages.txt",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_names(archive: zipfile.ZipFile) -> set[str]:
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise SystemExit("archive contains duplicate paths")
    for name in names:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or "\\" in name:
            raise SystemExit(f"archive contains unsafe path: {name}")
    return set(names)


def bytes_sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    runtime_path = args.output / "winq-emu-alpha10-portable.zip"
    source_path = args.output / "winq-emu-alpha10-source.zip"

    with zipfile.ZipFile(runtime_path) as runtime:
        names = safe_names(runtime)
        missing = REQUIRED - names
        if missing:
            raise SystemExit(f"runtime is missing: {', '.join(sorted(missing))}")
        if not any(name.startswith("LICENSES/qemu/") for name in names):
            raise SystemExit("runtime has no QEMU license")
        if not any(name.startswith("LICENSES/virglrenderer/") for name in names):
            raise SystemExit("runtime has no virglrenderer license")
        for executable in ("bin/qemu-system-x86_64.exe", "bin/qemu-system-x86_64w.exe"):
            if runtime.read(executable)[:2] != b"MZ":
                raise SystemExit(f"{executable} is not a Windows executable")
        lock = json.loads(runtime.read("provenance/sources.lock.json"))
        expected_lock = json.loads(
            Path(__file__).with_name("sources.lock.json").read_text(encoding="utf-8")
        )
        if lock != expected_lock:
            raise SystemExit("runtime source lock differs from the build recipe")
        manifest = json.loads(runtime.read("provenance/runtime-manifest.json"))
        for component in ("qemu", "virglrenderer"):
            if manifest["sources"][component]["commit"] != lock[component]["commit"]:
                raise SystemExit(f"{component} manifest does not match the source lock")
            patches = lock[component].get("patches", [])
            if manifest["sources"][component].get("patches", []) != patches:
                raise SystemExit(f"{component} manifest patches do not match the source lock")
            for entry in patches:
                shipped = f"provenance/{entry['file']}"
                if shipped not in names:
                    raise SystemExit(f"runtime provenance is missing {entry['file']}")
                if bytes_sha256(runtime.read(shipped)) != entry["sha256"]:
                    raise SystemExit(f"runtime provenance patch digest mismatch: {entry['file']}")
        described = set()
        for entry in manifest["files"]:
            name = entry["path"]
            if name in described or name not in names:
                raise SystemExit(f"manifest has an invalid file entry: {name}")
            described.add(name)
            value = runtime.read(name)
            if entry["size"] != len(value) or entry["sha256"] != bytes_sha256(value):
                raise SystemExit(f"manifest hash mismatch for {name}")
        expected_described = names - {"provenance/runtime-manifest.json"}
        if described != expected_described:
            raise SystemExit("manifest does not describe every runtime file")

    with zipfile.ZipFile(source_path) as source:
        names = safe_names(source)
        required_source_files = {
            "qemu/COPYING",
            "qemu/VERSION",
            "qemu/include/qemu/osdep.h",
            "virglrenderer/COPYING",
            "virglrenderer/meson.build",
            "build-recipe/archive.py",
            "build-recipe/build.sh",
            "build-recipe/sources.lock.json",
            "build-recipe/verify.py",
        }
        missing = required_source_files - names
        if missing:
            raise SystemExit(
                f"corresponding source archive is missing: {', '.join(sorted(missing))}"
            )
        source_lock = json.loads(source.read("build-recipe/sources.lock.json"))
        if source_lock != expected_lock:
            raise SystemExit("source archive lock differs from the build recipe")
        for component in ("qemu", "virglrenderer"):
            for entry in expected_lock[component].get("patches", []):
                shipped = f"build-recipe/{entry['file']}"
                if shipped not in names:
                    raise SystemExit(f"source archive is missing {entry['file']}")
                if bytes_sha256(source.read(shipped)) != entry["sha256"]:
                    raise SystemExit(
                        f"source archive patch digest mismatch: {entry['file']}"
                    )

    sums = {}
    for line in (args.output / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        # GNU coreutils uses a space followed by either another space for text
        # mode or an asterisk for binary mode. MSYS2 emits the latter for ZIPs.
        match = re.fullmatch(r"([0-9a-f]{64}) [ *]([^/]+)", line)
        if not match:
            raise SystemExit("SHA256SUMS has an invalid line")
        if match.group(2) in sums:
            raise SystemExit(f"SHA256SUMS has a duplicate entry for {match.group(2)}")
        sums[match.group(2)] = match.group(1)
    expected_sum_names = {runtime_path.name, source_path.name}
    if set(sums) != expected_sum_names:
        raise SystemExit("SHA256SUMS does not describe exactly the runtime and source archives")
    for path in (runtime_path, source_path):
        if sums.get(path.name) != sha256(path):
            raise SystemExit(f"SHA256 mismatch for {path.name}")

    print(f"ok - verified {len(REQUIRED)} required runtime files and corresponding source")


if __name__ == "__main__":
    main()
