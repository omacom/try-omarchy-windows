"""Retain direct-port evidence while rejecting incomplete/mixed broker routes."""
import copy
import hashlib
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock
from broker_runtime_evidence import broker_runtime_complete, runtime_transport_complete
from broker_runtime_evidence import broker_aware_initialization_complete as initialization_complete
from broker_runtime_qemu import LEGACY_SHA256, load_harness
import test_owned_runtime_qemu as legacy

# Project-owned receipt rows from the actual display-free run 3.
BROKER = '''KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false
BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied
BROKER_ROUTE_START protocol=3 clientPid=82 clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false
LINUX_BRIDGE transport=authenticated-broker nativeWorkerClients=1
BROKER_OBJECT_DRAIN producerExited=true retained=0
BROKER_ROUTE_EXIT processExited=true retainedViews=0 nativeCalls=1242 viewsCreated=173 viewsReleased=173 rejected=0 multiClient=false
BROKER_ROUTE_REAP exit=0
KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true
'''
INITIALIZATION = '''factory=00000000
list=00000000
nvidia vendor=4318
nvidiaUmdPresentDuringPrivateQuery=true
LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true
realDxgForwarding=false
device=00000000
copyQueue=00000000
BRIDGE_RUNTIME_EXIT=0
'''
CONTROL = dict(ownedQemuExited=True, qemuExit=0, qemuForcedStop=False, fenceControlFailed=False)
CLEANUP = dict(driverCleanupVerified=True)


class BrokerRuntimeEvidence(unittest.TestCase):
    def test_adapter_preserves_legacy_source_and_module_behavior(self):
        path = Path(legacy.__file__)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), LEGACY_SHA256)
        original = legacy.initialization_complete
        adapter = load_harness()
        self.assertIs(legacy.initialization_complete, original)
        self.assertIsNot(adapter, legacy)
        self.assertFalse(legacy.initialization_complete(INITIALIZATION + BROKER, 0, CONTROL, CLEANUP))
        self.assertTrue(adapter.initialization_complete(INITIALIZATION + BROKER, 0, CONTROL, CLEANUP))
        changed = path.read_bytes() + b'\nraise AssertionError("drift was executed")\n'
        with mock.patch('broker_runtime_qemu.Path.read_bytes', return_value=changed) as read:
            with self.assertRaises(ValueError): load_harness(path)
            read.assert_called_once()
        result = subprocess.run([sys.executable, str(path.with_name('broker_runtime_qemu.py')), '--help'],
                                capture_output=True, text=True, timeout=15,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--runtime-workload', result.stdout)

    def test_real_broker_receipts_and_legacy_direct_transport(self):
        self.assertTrue(broker_runtime_complete(BROKER))
        self.assertTrue(initialization_complete(INITIALIZATION + BROKER, 0, CONTROL, CLEANUP))
        self.assertTrue(initialization_complete(INITIALIZATION + 'transport=virtio-port\n', 0, CONTROL, CLEANUP))
        self.assertFalse(broker_runtime_complete('transport=virtio-port\n'))

    def test_each_missing_or_duplicate_broker_witness_is_rejected(self):
        for row in BROKER.splitlines(keepends=True):
            with self.subTest(row=row):
                missing = BROKER.replace(row, '', 1)
                # Removing transport cannot let initialization pass, either.
                self.assertFalse(initialization_complete(INITIALIZATION + missing, 0, CONTROL, CLEANUP))
                self.assertFalse(broker_runtime_complete(BROKER + row))

    def test_privilege_raw_access_leaks_failed_reap_and_mixed_routes_are_rejected(self):
        for old, new in (
            ('uid=65534 gid=65534', 'uid=0 gid=0'),
            ('clientUid=65534', 'clientUid=0'),
            ('protocol=3', 'protocol=2'),
            ('effectiveCapabilities=0', 'effectiveCapabilities=1'),
            ('noNewPrivs=true', 'noNewPrivs=false'),
            ('controllerOpen=denied', 'controllerOpen=allowed'),
            ('nativePortOpen=denied', 'nativePortOpen=allowed'),
            ('kernelControllersClient=false', 'kernelControllersClient=true'),
            ('rawSysfsFallback=false', 'rawSysfsFallback=true'),
            ('retainedViews=0', 'retainedViews=1'),
            ('viewsReleased=173', 'viewsReleased=172'),
            ('viewsCreated=173 viewsReleased=173', 'viewsCreated=0 viewsReleased=0'),
            ('nativeCalls=1242', 'nativeCalls=0'),
            ('rejected=0', 'rejected=1'),
            ('retained=0', 'retained=1'),
            ('multiClient=false', 'multiClient=true'),
            ('BROKER_ROUTE_REAP exit=0', 'BROKER_ROUTE_REAP exit=1'),
            ('mmioReads=0', 'mmioReads=1'),
            ('mmioWrites=0', 'mmioWrites=1'),
            ('unloaded=true', 'unloaded=false'),
        ):
            with self.subTest(change=(old, new)):
                value = BROKER.replace(old, new)
                self.assertFalse(broker_runtime_complete(value))
                self.assertFalse(initialization_complete(INITIALIZATION + value, 0, CONTROL, CLEANUP))
        self.assertFalse(runtime_transport_complete(BROKER + 'transport=virtio-port\n'))
        self.assertFalse(runtime_transport_complete(BROKER + 'LINUX_BRIDGE transport=unknown\n'))
        self.assertFalse(broker_runtime_complete(BROKER.replace('clientPid=82', 'clientPid=2147483648')))
        drain = 'BROKER_OBJECT_DRAIN producerExited=true retained=0\n'
        self.assertFalse(broker_runtime_complete(drain + BROKER.replace(drain, '')))

    def test_runtime_device_queue_and_owned_cleanup_are_still_required(self):
        for field in ('device', 'copyQueue'):
            self.assertFalse(initialization_complete(
                INITIALIZATION.replace(field + '=00000000', field + '=80004005') + BROKER,
                0, CONTROL, CLEANUP))
        for key, value in (('ownedQemuExited', False), ('qemuForcedStop', True),
                           ('qemuExit', 1), ('fenceControlFailed', True)):
            control = copy.deepcopy(CONTROL); control[key] = value
            self.assertFalse(initialization_complete(INITIALIZATION + BROKER, 0, control, CLEANUP))
        self.assertFalse(initialization_complete(INITIALIZATION + BROKER, 1, CONTROL, CLEANUP))
        self.assertFalse(initialization_complete(INITIALIZATION + BROKER, 0, CONTROL, dict(driverCleanupVerified=False)))


if __name__ == '__main__':
    unittest.main()
