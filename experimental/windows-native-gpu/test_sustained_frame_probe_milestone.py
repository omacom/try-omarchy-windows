import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


class SustainedFrameProbe(unittest.TestCase):
    def test_longer_budget_keeps_exact_rendering_and_input_sources(self):
        gpu=Path(__file__).resolve().parent
        def module(name):
            spec=importlib.util.spec_from_file_location(name.replace('-','_'),gpu/name/'generate.py')
            value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
        base=module('active-frame-probe');longer=module('sustained-frame-probe')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);a=base.generate(gpu,root/'a');b=longer.generate(gpu,root/'b')
            self.assertEqual(a['generatedSources'],b['generatedSources'])
            self.assertEqual((a['frameCount'],b['frameCount']),(300,1800))
            self.assertEqual(b['compiledFrameDefine'],'HYPRLAND_QUEUE_FRAMES=1800')
            self.assertEqual(a['workloadDeadlineSeconds'],b['workloadDeadlineSeconds'])
            for name in a['generatedSources']:self.assertEqual((root/'a'/name).read_bytes(),(root/'b'/name).read_bytes())
            self.assertFalse(b['renderingCodeChanged']);self.assertFalse(b['inputImplementationChanged'])
            with self.assertRaises(FileExistsError):longer.generate(gpu,root/'b')
            changed=root/'changed/active-frame-probe';changed.mkdir(parents=True)
            (changed/'generate.py').write_bytes((gpu/'active-frame-probe/generate.py').read_bytes()+b'\n')
            with self.assertRaises(ValueError):longer.generate(changed.parent,root/'invalid')
            self.assertFalse((root/'invalid').exists())
        self.assertEqual(hashlib.sha256((gpu/'active-frame-probe/generate.py').read_bytes()).hexdigest(),longer.BASE_GENERATOR)


if __name__=='__main__':unittest.main()
