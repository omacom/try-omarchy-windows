"""Source provenance controls for the active workload; no performance claim."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

path=Path(__file__).with_name('active-frame-probe')/'generate.py'
spec=importlib.util.spec_from_file_location('active_frame_generation',path)
generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)


class ActiveFrameProbe(unittest.TestCase):
    def test_complete_deterministic_derivation_preserves_baseline(self):
        gpu=Path(__file__).parent
        before={name:(gpu/name).read_bytes() for name in generator.BASE}
        with tempfile.TemporaryDirectory() as temporary:
            first=Path(temporary)/'first';second=Path(temporary)/'second'
            a=generator.generate(gpu,first);b=generator.generate(gpu,second)
            self.assertEqual(a,b)
            for name,digest in a['generatedSources'].items():
                self.assertEqual((first/name).read_bytes(),(second/name).read_bytes())
                self.assertEqual(hashlib.sha256((first/name).read_bytes()).hexdigest(),digest)
            with self.assertRaises(FileExistsError):generator.generate(gpu,first)
            self.assertFalse(a['nearNativePerformanceVerified'])
            self.assertFalse(a['inputImplementationChanged'])
        self.assertEqual(before,{name:(gpu/name).read_bytes() for name in before})

    def test_source_drift_is_rejected_before_any_output(self):
        gpu=Path(__file__).parent
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name in generator.BASE:(root/name).write_bytes((gpu/name).read_bytes())
            path=root/'hyprland_startup_probe.c';path.write_bytes(path.read_bytes()+b'\n')
            with self.assertRaises(ValueError):generator.generate(root,root/'output')
            self.assertFalse((root/'output').exists())


if __name__=='__main__':unittest.main()
