"""Native desktop staging preserves the pinned upstream entry except Exec."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

GPU=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('session_entry',GPU/'prepare_hyprland_session_entry.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
BASE=b'''[Desktop Entry]
Name=Hyprland
Comment=An intelligent dynamic tiling Wayland compositor
Exec=/usr/bin/start-hyprland
Type=Application
DesktopNames=Hyprland
Keywords=tiling;wayland;compositor;
'''
class SessionEntry(unittest.TestCase):
    def test_exact_inner_exec_replacement_retains_every_other_byte(self):
        self.assertEqual(hashlib.sha256(BASE).hexdigest(),module.BASE_SHA256)
        with tempfile.TemporaryDirectory(prefix='omarchy-entry-') as temporary:
            root=Path(temporary);source=root/'hyprland.desktop';source.write_bytes(BASE)
            receipt=module.generate(source,root/'stage')
            target=root/'stage/usr/local/share/wayland-sessions/hyprland.desktop'
            self.assertEqual(target.read_bytes().replace(module.NATIVE,module.ORIGINAL,1),BASE)
            self.assertEqual(receipt['changedFields'],['Exec']);self.assertTrue(receipt['watchdogRetained'])
            self.assertFalse(receipt['installed']);self.assertFalse(receipt['dailyUseAccepted'])
            self.assertEqual(list((root/'stage/usr/local/share/wayland-sessions').iterdir()),[target])
    def test_modified_upstream_is_rejected_before_output_creation(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-entry-') as temporary:
            root=Path(temporary);source=root/'hyprland.desktop';source.write_bytes(BASE+b'X-Changed=true\n')
            with self.assertRaises(ValueError):module.generate(source,root/'stage')
            self.assertFalse((root/'stage').exists())
    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-entry-') as temporary:
            root=Path(temporary);source=root/'hyprland.desktop';source.write_bytes(BASE)
            output=root/'stage';output.mkdir();(output/'keep').write_bytes(b'existing data')
            with self.assertRaises(FileExistsError):module.generate(source,output)
            self.assertEqual((output/'keep').read_bytes(),b'existing data')
if __name__=='__main__':unittest.main(verbosity=2)
