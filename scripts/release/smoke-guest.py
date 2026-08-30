#!/usr/bin/env python3
"""Boot the factory image and prove instant provisioning works."""

from __future__ import annotations

import argparse
import json
import os
import selectors
import subprocess
import sys
import time
from pathlib import Path


SUCCESS = b"TRYOMARCHY_SMOKE:omarchy:instant-trial"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()

    if not Path("/dev/kvm").exists() or not os.access("/dev/kvm", os.R_OK | os.W_OK):
        raise SystemExit("release smoke test requires accessible /dev/kvm")

    spec = json.loads((args.artifacts / "build-spec.json").read_text(encoding="utf-8"))
    cmdline = spec["runtime"]["kernelCommandLine"]
    cmdline = cmdline.replace("console=tty0 ", "").replace("console=hvc0", "console=ttyS0")
    cmdline += " tryomarchy.instant=1 systemd.unit=multi-user.target"

    command = [
        "qemu-system-x86_64",
        "-nodefaults",
        "-no-reboot",
        "-snapshot",
        "-accel",
        "kvm",
        "-machine",
        "q35",
        "-cpu",
        "host",
        "-smp",
        "4",
        "-m",
        "4096",
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "stdio",
        "-drive",
        f"file={args.artifacts / 'rootfs.ext4'},format=raw,if=virtio",
        "-kernel",
        str(args.artifacts / "vmlinuz-linux"),
        "-initrd",
        str(args.artifacts / "initramfs-linux.img"),
        "-append",
        cmdline,
        "-device",
        "virtio-rng-pci",
        "-netdev",
        "user,id=net0",
        "-device",
        "virtio-net-pci,netdev=net0",
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    assert process.stdin is not None and process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + args.timeout
    transcript = bytearray()
    login_attempts = 0
    sent_command = False
    password_sent_at: float | None = None
    password_offset = 0

    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            events = selector.select(timeout=1)
            for key, _ in events:
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    continue
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                transcript.extend(data)
                if len(transcript) > 1_000_000:
                    del transcript[:-500_000]

                if SUCCESS in transcript:
                    process.wait(timeout=90)
                    print("ok - instant guest reached a usable trial account")
                    return

                recent = bytes(transcript[-4096:])
                if recent.rstrip().endswith(b"login:") and login_attempts < 20:
                    process.stdin.write(b"omarchy\n")
                    process.stdin.flush()
                    login_attempts += 1
                    transcript.extend(b"\n")
                elif recent.rstrip().endswith(b"Password:"):
                    process.stdin.write(b"omarchy\n")
                    process.stdin.flush()
                    password_sent_at = time.monotonic()
                    password_offset = len(transcript)
                    transcript.extend(b"\n")

                if password_sent_at is not None and b"Login incorrect" in transcript[password_offset:]:
                    password_sent_at = None

                if (
                    password_sent_at is not None
                    and not sent_command
                    and time.monotonic() - password_sent_at >= 3
                ):
                    process.stdin.write(
                        b"printf 'TRYOMARCHY_SMOKE:%s:%s\\n' \"$(id -un)\" "
                        b"\"$(cat /var/lib/try-omarchy/provision-mode 2>/dev/null)\"; "
                        b"sudo systemctl poweroff\n"
                    )
                    process.stdin.flush()
                    sent_command = True
                    transcript.extend(b"\n")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    tail = bytes(transcript[-8000:]).decode("utf-8", errors="replace")
    raise SystemExit(f"instant guest smoke test failed\n\n{tail}")


if __name__ == "__main__":
    main()
