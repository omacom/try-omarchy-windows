"""CPU/data controls only. Synthetic files are never loaded as libraries."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


payload = module('desktop_payload', HERE / 'initcpio/desktop_payload.py')
build = module('record_build', HERE.parent / 'mesa/record_build.py')


class DesktopData(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'VERSION').write_text('26.2.6\n')
        self.recipe = self.root / 'recipe'
        self.recipe.mkdir()
        manifest = json.loads((HERE.parent / 'mesa/versions/26.2.6/recipe.json').read_text())
        (self.recipe / 'recipe.json').write_text(json.dumps(manifest))
        self.stage = self.root / 'stage'
        for name in manifest['runtimeRequiredPaths']:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name not in manifest['runtimeAliases']:
                path.write_bytes(b'CPU fixture; not an ELF\\n' + name.encode())
        for name, target in manifest['runtimeAliases'].items():
            (self.stage / name).symlink_to(target)
        icd = self.stage / 'share/vulkan/icd.d/dzn_icd.x86_64.json'
        icd.parent.mkdir(parents=True, exist_ok=True)
        icd.write_text(json.dumps({'ICD': {'library_path': '/usr/lib/libvulkan_dzn.so'}}))
        (self.stage / 'include').mkdir()
        (self.stage / 'include/gbm.h').write_text('Build-only SDK header; not shipped')
        build.record(self.source, self.stage, self.recipe)

    def archive(self):
        path = self.root / 'payload.tar'
        with tarfile.open(path, 'w', dereference=False) as tar:
            for name in ('lib', 'share'):
                tar.add(self.stage / name, arcname=name)
        return path

    def extract(self):
        return payload.extract(self.archive(), self.root / 'installed', self.recipe / 'recipe.json')

    def test_valid_cohort(self):
        self.assertEqual(self.extract(), '26.2.6')
        # The session user loads these libraries: the staged directory must
        # not keep mkdtemp's owner-only mode.
        self.assertEqual((self.root / 'installed').stat().st_mode & 0o055, 0o055)
        data = json.loads((self.root / 'installed/share/vulkan/icd.d/dzn_icd.x86_64.json').read_text())
        self.assertEqual(data['ICD']['library_path'], str(self.root / 'installed/lib/libvulkan_dzn.so'))
        self.assertEqual((self.root / 'installed/lib/libgbm.so').read_bytes(),
                         (self.root / 'installed/lib/libgbm.so.1').read_bytes())

    def test_corrupt_component(self):
        (self.stage / 'lib/libgbm.so.1').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Changed component'):
            self.extract()
        self.assertFalse((self.root / 'installed').exists())

    def test_same_size_corruption_before_publication(self):
        path = self.stage / 'lib/libvulkan_dzn.so'
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        with self.assertRaisesRegex(ValueError, 'Changed component'):
            self.extract()
        self.assertFalse((self.root / 'installed').exists())

    def test_receipt_covers_only_shipped_roots(self):
        inventory = json.loads((self.stage / 'lib/virtio-dxg-build.json').read_text())['files']
        self.assertNotIn('include/gbm.h', inventory)
        self.assertTrue(inventory)
        self.assertTrue(all(name.split('/', 1)[0] in ('lib', 'share') for name in inventory))

    def test_missing_gpu_loader_rejected_even_with_matching_inventory(self):
        name = 'lib/dri/d3d12_dri.so'
        (self.stage / name).unlink()
        receipt = self.stage / 'lib/virtio-dxg-build.json'
        data = json.loads(receipt.read_text())
        del data['files'][name]
        receipt.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'Incomplete graphics cohort'):
            self.extract()

    def test_wrong_loader_alias_rejected_at_build(self):
        path = self.stage / 'lib/dri/d3d12_dri.so'
        path.unlink()
        path.symlink_to('../libgallium-26.2.6.so')
        with self.assertRaisesRegex(ValueError, 'Incorrect runtime alias'):
            build.record(self.source, self.stage, self.recipe)

    def test_partial_cohort(self):
        (self.stage / 'lib/libEGL_mesa.so.0').unlink()
        with self.assertRaisesRegex(ValueError, 'inventory differs'):
            self.extract()

    def test_mismatched_recipe(self):
        with (self.recipe / 'recipe.json').open('a') as stream:
            stream.write('\n')
        with self.assertRaisesRegex(ValueError, 'recipes differ'):
            self.extract()

    def test_wrong_version_label(self):
        (self.stage / 'lib/virtio-dxg-mesa-version').write_text('26.2.4')
        with self.assertRaisesRegex(ValueError, 'version label'):
            self.extract()

    def test_no_receipt(self):
        (self.stage / 'lib/virtio-dxg-build.json').unlink()
        with self.assertRaisesRegex(ValueError, 'receipt'):
            self.extract()

    def test_existing_destination(self):
        (self.root / 'installed').mkdir()
        with self.assertRaisesRegex(ValueError, 'Destination exists'):
            self.extract()

    def test_symlink_escape(self):
        link = self.stage / 'lib/libgbm.so'
        link.unlink()
        link.symlink_to('../../outside')
        with self.assertRaises((ValueError, FileNotFoundError)):
            build.record(self.source, self.stage, self.recipe)

    def test_unsafe_names(self):
        for name in ('', '.', '/lib/x', 'lib/../x', 'lib//x', 'lib\\x', 'C:/x', 'etc/passwd'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                payload.canonical(name)

    def test_desktop_actions_and_urls(self):
        original = ('[Desktop Entry]\nName=Chromium\nName[fr]=Navigateur\n'
                    'Exec=/usr/bin/chromium %U\nTryExec=chromium\nActions=new-window;\n'
                    '[Desktop Action new-window]\nExec=chromium --new-window %U\n')
        changed = payload.desktop_entry(original)
        self.assertEqual(changed.count('Exec=/run/virtio-dxg/bin/chromium'), 3)
        self.assertIn('--new-window %U\n', changed)
        self.assertIn('Name[fr]=Navigateur\n', changed)

    def test_unknown_desktop_not_rewritten(self):
        with self.assertRaises(ValueError):
            payload.desktop_entry('Exec=/opt/another-browser %U\n')

    def test_overlay_preparation_is_data_only(self):
        binary = self.root / 'usr/bin/chromium'
        binary.parent.mkdir(parents=True)
        binary.write_text('not executed')
        desktop = self.root / 'usr/share/applications/chromium.desktop'
        desktop.parent.mkdir(parents=True)
        desktop.write_text('Exec=/usr/bin/chromium %U\n')
        self.assertTrue(payload.prepare_browser(self.root, HERE / 'initcpio/chromium-wrapper'))
        self.assertIn('lowerdir=/run/virtio-dxg/applications:',
                      (self.root / 'run/systemd/system/virtio-dxg-bin.service').read_text())
        self.assertTrue((self.root / 'run/systemd/system/sysinit.target.wants/virtio-dxg-bin.service').is_symlink())

    def test_wrapper_arguments_without_browser(self):
        # Only redirect exec to an owned argv recorder. No browser starts.
        stub = self.root / 'argv-recorder'
        stub.write_text('#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
        stub.chmod(0o755)
        wrapper = self.root / 'chromium-wrapper'
        wrapper.write_text((HERE / 'initcpio/chromium-wrapper').read_text().replace(
            'exec /usr/bin/chromium', 'exec ' + str(stub)))
        arguments = ['--new-window', 'https://example.test/a b?c=1&d=2']
        for display, passthrough, disable in [(' :1', '', False), ('', '', False),
                                             (':1', 'passthrough', False), (':1', '', True)]:
            argv = arguments + (['--disable-gpu'] if disable else [])
            env = dict(os.environ, DISPLAY=display, VIRTIO_DXG_CHROMIUM=passthrough)
            result = subprocess.run(['sh', str(wrapper), *argv], env=env,
                                    capture_output=True, text=True, check=True, timeout=5)
            observed = json.loads(result.stdout)
            self.assertEqual(observed[:len(argv)], argv)
            self.assertNotIn('--no-sandbox', observed)
            if display and not passthrough and not disable:
                self.assertIn('--use-gl=angle', observed)
                self.assertIn('--disable-accelerated-video-encode', observed)
            else:
                self.assertEqual(observed, argv)


if __name__ == '__main__':
    unittest.main()
