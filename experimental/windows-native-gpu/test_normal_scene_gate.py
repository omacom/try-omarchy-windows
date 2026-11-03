"""Synthetic metadata controls; no GPU, VM, desktop input or hardware proof."""
import copy
import json
import unittest

from normal_scene_gate import MAX_METADATA_BYTES, assess_scene_metadata, captured_scene_tables


class SceneGateTests(unittest.TestCase):
    def setUp(self):
        self.monitors = [dict(name='WDDM-1', width=3840, height=2160, scale=1)]
        self.clients = [dict(pid=17, mapped=True, **{'class': 'foot'})]
        self.layers = {'WDDM-1': {'levels': {
            '0': [dict(namespace='fixture-background', w=3840, h=2160)],
            '1': [], '2': [dict(namespace='fixture-panel', w=3840, h=42)], '3': []}}}

    def result(self, pid=17, ping=True):
        return assess_scene_metadata(self.monitors, self.clients, self.layers, pid, ping)

    def test_candidates_never_accept_health_or_daily_use(self):
        result = self.result()
        self.assertTrue(result['prerequisitesMet'])
        self.assertTrue(result['visualInspectionRequired'])
        for key in ('healthyDesktopAccepted', 'hostDriverStabilityAccepted', 'dailyUseAccepted'):
            self.assertFalse(result[key])

    def test_ping_and_terminal_with_four_plain_title_crash_reporters(self):
        self.layers['WDDM-1']['levels'] = {str(n): [] for n in range(4)}
        self.clients += [dict(pid=20+n, mapped=True, title='quickshell',
                              **{'class': 'org.quickshell'}) for n in range(4)]
        result = self.result()
        self.assertTrue(result['shellPing'] and result['mappedFoot'])
        self.assertEqual(result['unexpectedQuickshellTopLevels'], 4)
        self.assertEqual(result['backgroundLayerCandidates'], 0)
        self.assertEqual(result['panelLayerCandidates'], 0)
        self.assertFalse(result['prerequisitesMet'])

    def test_reporter_rejected_even_with_layers(self):
        self.clients.append(dict(pid=18, mapped=True, **{'class': 'org.quickshell'}))
        self.assertFalse(self.result()['prerequisitesMet'])

    def test_correct_terminal_identity_and_ping_required(self):
        self.assertFalse(self.result(pid=18)['prerequisitesMet'])
        self.assertFalse(self.result(ping=False)['prerequisitesMet'])
        self.clients[0]['mapped'] = False
        self.assertFalse(self.result()['prerequisitesMet'])

    def test_bool_is_not_a_process_identity(self):
        self.clients[0]['pid'] = True
        self.assertFalse(self.result(pid=1)['mappedFoot'])
        with self.assertRaises(ValueError):
            self.result(pid=True)

    def test_background_and_panel_both_required(self):
        self.layers['WDDM-1']['levels']['0'] = []
        self.assertFalse(self.result()['prerequisitesMet'])
        self.setUp()
        self.layers['WDDM-1']['levels']['2'] = []
        self.assertFalse(self.result()['prerequisitesMet'])

    def test_logical_scale(self):
        for scale in (1.25, 2, 4):
            with self.subTest(scale=scale):
                layers = copy.deepcopy(self.layers)
                monitor = copy.deepcopy(self.monitors)
                monitor[0]['scale'] = scale
                for level in layers['WDDM-1']['levels'].values():
                    for layer in level:
                        layer['w'] /= scale
                        layer['h'] /= scale
                self.assertTrue(assess_scene_metadata(monitor, self.clients, layers, 17, True)['prerequisitesMet'])

    def test_invalid_scale_and_dimensions(self):
        for scale in (False, 0, -1, float('nan'), float('inf'), 10**400, 9):
            with self.subTest(scale=str(scale)[:20]):
                self.monitors[0]['scale'] = scale
                with self.assertRaises(ValueError):
                    self.result()
        self.setUp()
        self.monitors[0]['width'] = 3840.0
        with self.assertRaises(ValueError):
            self.result()

    def test_bad_layers_do_not_become_candidates(self):
        for value in (False, 0, -1, float('nan'), float('inf'), 10**400):
            with self.subTest(value=str(value)[:20]):
                self.layers['WDDM-1']['levels']['2'][0]['w'] = value
                self.assertFalse(self.result()['prerequisitesMet'])
        self.setUp()
        self.layers['WDDM-1']['levels']['0'][0]['namespace'] = ' '
        self.assertFalse(self.result()['prerequisitesMet'])

    def test_metadata_bounds(self):
        for target in ('clients', 'layers'):
            self.setUp()
            if target == 'clients':
                self.clients = self.clients * 129
            else:
                self.layers['WDDM-1']['levels']['3'] = [{}] * 129
            with self.assertRaises(ValueError):
                self.result()
        self.setUp()
        self.layers['WDDM-1']['levels']['4'] = []
        with self.assertRaises(ValueError):
            self.result()

    def test_three_tables_preserved(self):
        tables = [self.monitors, self.clients, self.layers]
        payload = 'TERMINAL_PID=17\nNORMAL_FRAME_CAPTURED\n' + '\n'.join(map(json.dumps, tables))
        self.assertEqual(captured_scene_tables(payload), tables)

    def test_bad_payloads_rejected(self):
        for payload in ('[]', 'NORMAL_FRAME_CAPTURED\n[]',
                        'NORMAL_FRAME_CAPTURED\n[] [] {} junk',
                        'NORMAL_FRAME_CAPTURED\n[] [] {"x":NaN}',
                        'NORMAL_FRAME_CAPTURED\n' * 2,
                        'NORMAL_FRAME_CAPTURED\n' + '[' * 2000,
                        'NORMAL_FRAME_CAPTURED\n' + 'x' * MAX_METADATA_BYTES,
                        'NORMAL_FRAME_CAPTURED\n' + '\u20ac' * (MAX_METADATA_BYTES // 2)):
            with self.subTest(length=len(payload)):
                with self.assertRaises(ValueError):
                    captured_scene_tables(payload)


if __name__ == '__main__':
    unittest.main()
