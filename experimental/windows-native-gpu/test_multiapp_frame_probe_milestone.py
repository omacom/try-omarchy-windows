"""Keep rendering/actions/input fixed while explicitly selecting the new ABI."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


class MultiappProfileFixture(unittest.TestCase):
    def setUp(self): self.gpu=Path(__file__).resolve().parent

    def module(self,directory,filename='generate.py'):
        spec=importlib.util.spec_from_file_location(directory.replace('-','_')+filename[:-3],self.gpu/directory/filename)
        value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

    def test_hook_diff_only_selects_profile14_and_its_entry_point(self):
        old=self.module('multiwindow-frame-probe');new=self.module('multiapp-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);before=old.generate(self.gpu,root/'old');after=new.generate(self.gpu,root/'new')
            self.assertEqual(after['multiappBaselineGeneratedSources'],before['generatedSources'])
            for name in before['generatedSources']:
                original=(root/'old'/name).read_text(encoding='utf-8')
                changed=(root/'new'/name).read_text(encoding='utf-8')
                if name=='active_probe.c':
                    changed=changed.replace(new.PROFILE_AFTER,new.PROFILE_BEFORE,1).replace(new.ENTRY_AFTER,new.ENTRY_BEFORE,1)
                self.assertEqual(original,changed,name)
            self.assertTrue(after['cpuMappingGuardRaised']);self.assertFalse(after['gpuUsageGuardRaised'])
            self.assertFalse(after['inputImplementationChanged']);self.assertFalse(after['renderingCodeChanged'])
            with self.assertRaises(FileExistsError):new.generate(self.gpu,root/'new')

    def test_service_diff_only_selects_marker_gated_profile14(self):
        old=self.module('multiwindow-frame-probe','generate_service.py');new=self.module('multiapp-frame-probe','generate_service.py')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);before=old.generate(self.gpu,root/'old');after=new.generate(self.gpu,root/'new')
            self.assertEqual(after['multiappBaselineGeneratedSources'],before['generatedSources'])
            for name in before['generatedSources']:
                original=(root/'old'/name).read_text(encoding='utf-8');changed=(root/'new'/name).read_text(encoding='utf-8')
                if name=='normal_root_graphics_service.c':changed=changed.replace(new.AFTER,new.BEFORE,1)
                self.assertEqual(original,changed,name)
            self.assertFalse(after['traceTransportChanged']);self.assertFalse(after['inputImplementationChanged'])
            self.assertTrue(after['gpuProtocolChanged'])

    def test_modified_parent_is_rejected_before_output(self):
        new=self.module('multiapp-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);folder=root/'gpu/multiwindow-frame-probe';folder.mkdir(parents=True)
            (folder/'generate.py').write_bytes((self.gpu/'multiwindow-frame-probe/generate.py').read_bytes()+b'\n')
            with self.assertRaises(ValueError):new.generate(folder.parent,root/'invalid')
            self.assertFalse((root/'invalid').exists())


if __name__=='__main__':unittest.main()
