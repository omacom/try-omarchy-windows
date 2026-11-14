"""Preserve the single-window renderer/input while adding a second real client."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


class MultiwindowFrameProbe(unittest.TestCase):
    def setUp(self):self.gpu=Path(__file__).resolve().parent

    def module(self,name):
        spec=importlib.util.spec_from_file_location(name.replace('-','_'),self.gpu/name/'generate.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

    def test_original_application_queue_and_input_are_unchanged(self):
        baseline=self.module('sustained-frame-probe');multi=self.module('multiwindow-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);old=baseline.generate(self.gpu,root/'old');new=multi.generate(self.gpu,root/'new')
            self.assertEqual(new['baselineGeneratedSources'],old['generatedSources'])
            for name in ('active_queue_probe.h','terminal_pulse.c','gpu-terminal-scene.sh'):
                self.assertEqual((root/'old'/name).read_bytes(),(root/'new'/name).read_bytes())
            before=(root/'old/active_probe.c').read_text(encoding='utf-8')
            after=(root/'new/active_probe.c').read_text(encoding='utf-8')
            start='static int input_delivery_enabled(void)';end='static int wait_gpu_retirement(pid_t pid)'
            self.assertEqual(before[before.index(start):before.index(end)],after[after.index(start):after.index(end)])
            self.assertFalse(new['inputImplementationChanged']);self.assertFalse(new['renderingCodeChanged'])
            self.assertEqual(new['frameCount'],1800)
            self.assertEqual(new['multiwindowProbe']['simultaneousVisibleWindows'],2)
            self.assertFalse(new['multiwindowProbe']['hostBackendChanged'])
            self.assertIn('"/tmp/omarchy-companion-terminal-pulse.json"',(root/'new/companion_pulse.c').read_text())
            self.assertNotIn('"/tmp/omarchy-active-terminal-pulse.json"',(root/'new/companion_pulse.c').read_text())
            self.assertEqual((root/'new/multiwindow_control.h').read_bytes(),
                (self.gpu/'multiwindow-frame-probe/multiwindow_control.h').read_bytes())
            for name,digest in new['generatedSources'].items():
                self.assertEqual(hashlib.sha256((root/'new'/name).read_bytes()).hexdigest(),digest)
            with self.assertRaises(FileExistsError):multi.generate(self.gpu,root/'new')

    def test_changed_baseline_rejected_before_output(self):
        multi=self.module('multiwindow-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);changed=root/'gpu/sustained-frame-probe';changed.mkdir(parents=True)
            (changed/'generate.py').write_bytes((self.gpu/'sustained-frame-probe/generate.py').read_bytes()+b'\n')
            with self.assertRaises(ValueError):multi.generate(changed.parent,root/'invalid')
            self.assertFalse((root/'invalid').exists())

    def test_service_observations_preserve_transport_and_execution_logic(self):
        baseline=self.module('virtio-trace-probe')
        spec=importlib.util.spec_from_file_location('multiwindow_service',self.gpu/'multiwindow-frame-probe/generate_service.py')
        service=importlib.util.module_from_spec(spec);spec.loader.exec_module(service)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);old=baseline.generate(self.gpu,root/'old');new=service.generate(self.gpu,root/'new')
            self.assertEqual(new['baselineGeneratedSources'],old['generatedSources'])
            for name in ('trace_port.h','trace_session.h'):
                self.assertEqual((root/'old'/name).read_bytes(),(root/'new'/name).read_bytes())
            before=(root/'old/normal_root_graphics_service.c').read_text(encoding='utf-8')
            after=(root/'new/normal_root_graphics_service.c').read_text(encoding='utf-8')
            start=after.index('    // Read-only replay of the admission snapshot')
            end=after.index('    if(native_render_loaded && native_render_stop()) result=125;',start)
            recovered=(after[:start]+after[end:]).replace(',"-t","omarchy-gpu-companion"','')
            self.assertEqual(before,recovered)
            self.assertFalse(new['traceTransportChanged']);self.assertFalse(new['inputImplementationChanged'])
            self.assertFalse(new['gpuProtocolChanged'])
            with self.assertRaises(FileExistsError):service.generate(self.gpu,root/'new')


if __name__=='__main__':unittest.main()
