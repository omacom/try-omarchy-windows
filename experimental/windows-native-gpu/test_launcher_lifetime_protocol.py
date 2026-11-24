"""Reject identity ambiguity at the real hidden runtime consumer boundary."""
import unittest
from launcher_lifetime_runtime_qemu import validate_ready


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.launcher_time = 134000000000000001
        self.qemu_time = self.launcher_time + 123
        self.message = dict(nativeLauncherVmReady=True, protocol=1, launchMode='backend-owned',
            bridgePid=102, launcherPid=101, launcherCreationTime100ns=str(self.launcher_time),
            qemuPid=103, qemuCreatorPid=102, qemuCreationTime100ns=str(self.qemu_time),
            heldProcessHandle=True, privateJobAssigned=True, imageFilePinned=True, qmpReady=True,
            guestGraphicsVerified=False, launcherInputVerified=False, dailyUseAccepted=False)

    def check(self, message, **changes):
        arguments = dict(bridge_pid=102, launcher_pid=101, launcher_time=self.launcher_time,
            qemu_pid=103, qemu_time=self.qemu_time, qemu_parent=102)
        arguments.update(changes)
        validate_ready(message, **arguments)

    def test_exact_held_identity(self):
        self.check(self.message)

    def test_wrong_or_missing_fields(self):
        mutations = dict(protocol=2, launchMode='launcher-owned', bridgePid=104, launcherPid=104,
            launcherCreationTime100ns=str(self.launcher_time + 1), qemuPid=104, qemuCreatorPid=104,
            qemuCreationTime100ns=str(self.qemu_time + 1), heldProcessHandle=False,
            privateJobAssigned=False, imageFilePinned=False, qmpReady=False,
            guestGraphicsVerified=True, launcherInputVerified=True, dailyUseAccepted=True)
        for key, value in mutations.items():
            with self.subTest(field=key):
                with self.assertRaises(RuntimeError): self.check(dict(self.message, **{key: value}))
        for key in self.message:
            with self.subTest(missing=key):
                altered = dict(self.message); del altered[key]
                with self.assertRaises(RuntimeError): self.check(altered)
        with self.assertRaises(RuntimeError): self.check(dict(self.message, futureClaim=True))

    def test_lossy_timestamps_and_boolean_integer_aliases(self):
        for key, value in [('launcherCreationTime100ns', float(self.launcher_time)),
                ('qemuCreationTime100ns', self.qemu_time), ('protocol', True),
                ('nativeLauncherVmReady', 1), ('guestGraphicsVerified', 0), ('qemuPid', 103.0)]:
            with self.subTest(field=key):
                with self.assertRaises(RuntimeError): self.check(dict(self.message, **{key: value}))

    def test_held_observation_disagrees(self):
        for changes in [dict(qemu_parent=104), dict(qemu_time=self.qemu_time + 1),
                dict(launcher_time=self.launcher_time + 1), dict(bridge_pid=104), dict(qemu_pid=104)]:
            with self.subTest(observation=changes):
                with self.assertRaises(RuntimeError): self.check(self.message, **changes)

    def test_matching_claim_cannot_identify_self_or_old_child(self):
        for pid in (0, 101, 102):
            with self.assertRaises(RuntimeError): self.check(dict(self.message, qemuPid=pid), qemu_pid=pid)
        old = self.launcher_time - 1
        with self.assertRaises(RuntimeError): self.check(dict(self.message, qemuCreationTime100ns=str(old)), qemu_time=old)


if __name__ == '__main__': unittest.main()
