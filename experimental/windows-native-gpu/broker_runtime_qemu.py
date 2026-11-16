"""Explicit broker-aware runtime adapter; preserves the historical harness bytes."""
import hashlib
import importlib.util
from pathlib import Path
from broker_runtime_evidence import broker_aware_initialization_complete

LEGACY_SHA256 = 'e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286'


def load_harness(path=None):
    path = Path(path) if path is not None else Path(__file__).with_name('test_owned_runtime_qemu.py')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != LEGACY_SHA256:
        raise ValueError('Broker adapter requires the exact historical runtime harness')
    spec = importlib.util.spec_from_file_location('_owned_broker_runtime_adapter', path)
    module = importlib.util.module_from_spec(spec)
    # Execute the bytes that were verified, without reopening a mutable path.
    exec(compile(raw, str(path), 'exec'), module.__dict__)
    # This separate module instance leaves historical imports and replays intact.
    # All GPU/cleanup/parser behavior comes from the unchanged original source.
    module.initialization_complete = broker_aware_initialization_complete
    return module


if __name__ == '__main__':
    raise SystemExit(load_harness().main())
