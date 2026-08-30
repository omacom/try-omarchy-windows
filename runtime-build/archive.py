#!/usr/bin/env python3
"""Create a sorted ZIP with fixed timestamps and stable file metadata."""

from __future__ import annotations

import argparse
import os
import stat
import time
import zipfile
from pathlib import Path


def zip_timestamp(epoch: int) -> tuple[int, int, int, int, int, int]:
    value = time.gmtime(max(epoch, 315532800))
    return value[:6]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--epoch", type=int, required=True)
    args = parser.parse_args()
    root = args.source.resolve()
    timestamp = zip_timestamp(args.epoch)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    paths = sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix())
    with zipfile.ZipFile(
        args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for path in paths:
            relative = path.relative_to(root).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISDIR(mode):
                continue
            info = zipfile.ZipInfo(relative, timestamp)
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            if stat.S_ISLNK(mode):
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(info, os.readlink(path).encode())
                continue
            info.external_attr = (stat.S_IFREG | (mode & 0o777)) << 16
            with path.open("rb") as source:
                archive.writestr(info, source.read())


if __name__ == "__main__":
    main()
