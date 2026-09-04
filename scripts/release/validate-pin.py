#!/usr/bin/env python3
"""Validate the launcher's independently pinned release manifest."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


TAG_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-preview)?$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
REQUIRED = {
    "build-spec.json",
    "guest-manifest.json",
    "initramfs-linux.img",
    "rootfs.ext4",
    "rootfs.ext4.zst",
    "vmlinuz-linux",
    "winq-emu-alpha10-portable.zip",
}


def source_value(source: str, name: str) -> str:
    match = re.search(rf'\b{name}\s*=\s*"([^"]+)"', source)
    if not match:
        raise SystemExit(f"could not find {name} in app/manifest.go")
    return match.group(1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("tag")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--repository", default="omacom/try-omarchy-windows")
    args = parser.parse_args()

    if not TAG_RE.fullmatch(args.tag):
        raise SystemExit(f"invalid release tag: {args.tag}")

    source = (args.root / "app/manifest.go").read_text(encoding="utf-8")

    update_source = (args.root / "app/update.go").read_text(encoding="utf-8")
    current_version = source_value(update_source, "currentVersion")
    if current_version != args.tag:
        raise SystemExit(f"currentVersion is {current_version}, expected {args.tag}")
    update_key = source_value(update_source, "updatePublicKeyHex")
    signer_source = (args.root / "app/cmd/sign-update/main.go").read_text(encoding="utf-8")
    signer_key = source_value(signer_source, "expectedPublicKeyHex")
    if update_key != signer_key or not SHA_RE.fullmatch(update_key):
        raise SystemExit("update signer key does not match the launcher trust root")
    release_url = source_value(source, "defaultReleaseURL")
    expected_url = f"https://github.com/{args.repository}/releases/download/{args.tag}"
    if release_url != expected_url:
        raise SystemExit(f"defaultReleaseURL is {release_url}, expected {expected_url}")

    expected_digest = source_value(source, "defaultSumsSHA256")
    if not SHA_RE.fullmatch(expected_digest):
        raise SystemExit("defaultSumsSHA256 is invalid")

    embed = re.search(r"//go:embed\s+([^\s]+)", source)
    if not embed:
        raise SystemExit("could not find embedded manifest fixture")
    fixture = args.root / "app" / embed.group(1)
    if fixture.name != f"SHA256SUMS.{args.tag}":
        raise SystemExit(f"embedded fixture name does not match {args.tag}")
    data = fixture.read_bytes()
    actual_digest = hashlib.sha256(data).hexdigest()
    if actual_digest != expected_digest:
        raise SystemExit(
            f"embedded manifest digest is {actual_digest}, expected {expected_digest}"
        )

    entries: dict[str, str] = {}
    for line_number, line in enumerate(data.decode("utf-8").splitlines(), 1):
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\\s]+)", line)
        if not match:
            raise SystemExit(f"invalid manifest line {line_number}")
        digest, name = match.groups()
        if name in entries:
            raise SystemExit(f"duplicate manifest entry: {name}")
        entries[name] = digest
    missing = sorted(REQUIRED - entries.keys())
    if missing:
        raise SystemExit(f"manifest is missing: {', '.join(missing)}")

    print(f"ok - {args.tag} pins authenticated manifest {actual_digest}")


if __name__ == "__main__":
    main()
