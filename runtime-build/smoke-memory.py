#!/usr/bin/env python3
"""Prove RAM survives a stopped source process and a fresh receiving QEMU."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def socket_address(address):
    return {"channels": [{"channel-type": "main", "addr": {
        "transport": "socket", "type": "inet", "host": address[0], "port": str(address[1]),
    }}]}


def reserve():
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    return server


class VM:
    def __init__(self, qemu, extra, *, env=None):
        self.qemu = qemu
        self.extra = extra
        self.env = env
        self.process = self.connection = self.stream = self.log = None
        self.sequence = 0

    def __enter__(self):
        with reserve() as reservation:
            address = reservation.getsockname()
        self.log = tempfile.TemporaryFile()
        firmware = self.qemu.parent / "share"
        args = [str(self.qemu)]
        if firmware.is_dir():
            args += ["-L", str(firmware)]
        args += ["-machine", "q35,accel=tcg", "-m", "32", "-nodefaults", "-display", "none", "-S", "-qmp", f"tcp:{address[0]}:{address[1]},server=on,wait=off", *self.extra]
        self.process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=self.log, env=self.env)
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and self.process.poll() is None:
                try:
                    self.connection = socket.create_connection(address, timeout=2)
                    break
                except OSError:
                    time.sleep(.05)
            if self.connection is None:
                raise RuntimeError("QEMU did not open its control socket")
            self.connection.settimeout(20)
            self.stream = self.connection.makefile("rb")
            if "QMP" not in json.loads(self.stream.readline()):
                raise RuntimeError("missing QMP greeting")
            self.call("qmp_capabilities")
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def call(self, command, arguments=None):
        self.sequence += 1
        request = {"execute": command, "id": self.sequence}
        if arguments is not None:
            request["arguments"] = arguments
        self.connection.sendall(json.dumps(request).encode() + b"\n")
        while True:
            reply = json.loads(self.stream.readline())
            if "event" in reply:
                continue
            if reply.get("id") != self.sequence or "error" in reply:
                raise RuntimeError(f"unexpected {command} result: {reply}")
            return reply["return"]

    def migrated(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            state = self.call("query-migrate")
            if state.get("status") == "completed":
                return
            if state.get("status") in {"failed", "cancelled"}:
                raise RuntimeError(f"migration failed: {state}")
            time.sleep(.05)
        raise RuntimeError("migration did not complete")

    def __exit__(self, *_):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=10)
        if self.stream is not None:
            self.stream.close()
        if self.connection is not None:
            self.connection.close()
        if self.log is not None:
            self.log.seek(0)
            detail = self.log.read().decode(errors="replace").strip()
            if detail:
                print(detail[-4096:])
            self.log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path)
    parser.add_argument("--pinch", action="store_true", help="include the dedicated pinch device")
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    devices = ["-device", "virtio-pinch-pci"] if args.pinch else []
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        seed = root / "seed.bin"
        pattern = b"unsaved application memory\x00\xff" * 1024
        seed.write_bytes(pattern)
        memory = root / "saved-memory.bin"
        with VM(qemu, devices + ["-device", f"loader,file={str(seed).replace(',', ',,')},addr=1048576,force-raw=on"]) as source, reserve() as server:
            server.listen(1)
            server.settimeout(20)

            def receive():
                connection, _ = server.accept()
                with connection, memory.open("wb") as output:
                    connection.settimeout(15)
                    total = 0
                    while True:
                        data = connection.recv(1 << 20)
                        if not data:
                            break
                        total += len(data)
                        if total > 128 << 20:
                            raise RuntimeError("unexpected memory stream size")
                        output.write(data)
                return total

            with ThreadPoolExecutor(max_workers=1) as executor:
                received = executor.submit(receive)
                source.call("migrate", socket_address(server.getsockname()))
                source.migrated()
                if received.result(timeout=20) <= 0:
                    raise RuntimeError("empty memory stream")
        # The original process is gone. No loader supplies the destination RAM.
        with VM(qemu, devices + ["-incoming", "defer"]) as target:
            with reserve() as reservation:
                address = reservation.getsockname()
            incoming = socket_address(address)
            incoming["exit-on-error"] = False
            target.call("migrate-incoming", incoming)
            with socket.create_connection(address, timeout=20) as connection, memory.open("rb") as saved:
                while data := saved.read(1 << 20):
                    connection.sendall(data)
                connection.shutdown(socket.SHUT_WR)
                target.migrated()
            state = target.call("query-status")
            if state["running"]:
                raise RuntimeError("restored guest started before validation")
            output = root / "restored-memory.bin"
            target.call("pmemsave", {"val": 1048576, "size": len(pattern), "filename": str(output)})
            if output.read_bytes() != pattern:
                raise RuntimeError("RAM contents changed across save and restore")
            target.call("cont")
            if not target.call("query-status")["running"]:
                raise RuntimeError("restored guest did not resume")
    print("ok - complete memory stream, source exit, restored RAM and explicit resume")


if __name__ == "__main__":
    main()
