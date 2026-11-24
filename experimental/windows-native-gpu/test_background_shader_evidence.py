"""Reject UI/input options, mismatched sampling and incomplete broker evidence."""
import copy
import unittest
from background_shader_evidence import FORBIDDEN, validate_background_controls
from test_broker_runtime_evidence import BROKER


class BackgroundShaderEvidence(unittest.TestCase):
    def setUp(self):
        self.source = dict(command=['python', 'harness', '--runtime-workload', 'native',
            '--driver-cpu-slots', '128', '--driver-fence-slots', '128'],
            sourceDiskAttached=False, guiFlagsAbsent=True, inputFlagsAbsent=True)
        self.guest = BROKER + 'LINUX_BRIDGE pagingFenceRetired=true direct=true loads=2 target=1 observed=1 operation=gpuva\n'

    def test_background_sampling_controls_preserve_both_explicit_selections(self):
        value = validate_background_controls(self.source, self.guest, [], 2)
        self.assertEqual(value['diagnosticReadTotal'], 2)
        self.assertFalse(value['presentationRequested'])
        value = validate_background_controls(self.source, self.guest.replace('loads=2 ', 'loads=10000 '), [], 10000)
        self.assertEqual(value['diagnosticReadTotal'], 10000)

    def test_each_foreground_input_or_disk_option_is_rejected(self):
        for option in FORBIDDEN:
            source = copy.deepcopy(self.source); source['command'].append(option)
            with self.subTest(option=option), self.assertRaises(ValueError):
                validate_background_controls(source, self.guest, [], 2)
        for key, value in (('sourceDiskAttached', True), ('guiFlagsAbsent', False), ('inputFlagsAbsent', False)):
            source = copy.deepcopy(self.source); source[key] = value
            with self.assertRaises(ValueError): validate_background_controls(source, self.guest, [], 2)

    def test_missing_duplicate_or_wrong_profile_and_malformed_reads_are_rejected(self):
        for option in ('--runtime-workload', '--driver-cpu-slots', '--driver-fence-slots'):
            for action in ('missing', 'duplicate', 'wrong'):
                source = copy.deepcopy(self.source); index = source['command'].index(option)
                if action == 'missing': del source['command'][index:index+2]
                elif action == 'duplicate': source['command'] += [option, '128']
                else: source['command'][index+1] = 'other'
                with self.subTest(option=option, action=action), self.assertRaises(ValueError):
                    validate_background_controls(source, self.guest, [], 2)
        for replacement in ('loads=10000 ', 'loads=0 ', 'loads=2invalid ', 'loads=02 '):
            with self.assertRaises(ValueError):
                validate_background_controls(self.source, self.guest.replace('loads=2 ', replacement), [], 2)
        with self.assertRaises(ValueError): validate_background_controls(self.source, BROKER, [], 2)
        with self.assertRaises(ValueError): validate_background_controls(self.source, self.guest, [], True)

    def test_even_failed_presentation_events_or_invalid_broker_receipts_are_rejected(self):
        for event in ('nativeSdlGraphicsChannelOpened', 'nativeSharedTexturePresented',
                      'nativeSharedTexturePresenterReleased', 'nativeInputChannelOpened'):
            with self.subTest(event=event), self.assertRaises(ValueError):
                validate_background_controls(self.source, self.guest, [{event: False}], 2)
        with self.assertRaises(ValueError):
            validate_background_controls(self.source, self.guest.replace('nativePortOpen=denied', 'nativePortOpen=allowed'), [], 2)


if __name__ == '__main__':
    unittest.main()
