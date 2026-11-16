"""Preserve frozen rendering/input sources around the pre-stream diagnostic."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


class PressureFrameProbe(unittest.TestCase):
    def setUp(self):
        self.gpu=Path(__file__).resolve().parent

    def module(self,name):
        spec=importlib.util.spec_from_file_location(name.replace('-','_'),self.gpu/name/'generate.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        return module

    def test_only_pre_stream_queue_insertion_changes(self):
        baseline=self.module('sustained-frame-probe');probe=self.module('pressure-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            before=baseline.generate(self.gpu,root/'baseline')
            after=probe.generate(self.gpu,root/'pressure')
            self.assertEqual(after['baselineGeneratedSources'],before['generatedSources'])
            self.assertEqual(after['sustainedBaselineGeneratorSha256'],probe.BASE_GENERATOR)
            self.assertEqual(after['frameCount'],1800)
            self.assertTrue(after['preStreamD3D12Pressure'])
            self.assertFalse(after['renderingCodeChanged']);self.assertFalse(after['inputImplementationChanged'])
            for name in before['generatedSources']:
                a=(root/'baseline'/name).read_bytes();b=(root/'pressure'/name).read_bytes()
                if name!='active_queue_probe.h':
                    self.assertEqual(a,b,name)
                    continue
                start=b.index(b'    if(expected==1) {\n')
                end=b.index(b'    void* fence=create(display,0x3144,NULL);',start)
                self.assertEqual(a,b[:start]+b[end:])
                self.assertIn(b'finish();',b[start:end])
                self.assertIn(b'atomic_compare_exchange_strong',b[start:end])
                self.assertIn(b'nativeObjectsReleased=true',b[start:end])
                self.assertNotIn(b'glReadPixels',b[start:end])
            for name,digest in after['pressureSources'].items():
                self.assertEqual(hashlib.sha256((self.gpu/name).read_bytes()).hexdigest(),digest)
            with self.assertRaises(FileExistsError):probe.generate(self.gpu,root/'pressure')

    def test_changed_baseline_rejected_before_output(self):
        probe=self.module('pressure-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);changed=root/'gpu/sustained-frame-probe';changed.mkdir(parents=True)
            (changed/'generate.py').write_bytes((self.gpu/'sustained-frame-probe/generate.py').read_bytes()+b'\n')
            with self.assertRaises(ValueError):probe.generate(changed.parent,root/'output')
            self.assertFalse((root/'output').exists())


if __name__=='__main__':unittest.main()
