import os
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import app_compat


def graphics(renderer="virgl (NVIDIA GeForce)", version="4.2", blocks=13, extensions=None):
    if extensions is None:
        extensions = app_compat.GL43_EXTENSIONS
    return (f"OpenGL renderer string: {renderer}\n"
            f"OpenGL core profile version string: {version} (Core Profile) Mesa\n"
            f"GL_MAX_VERTEX_UNIFORM_BLOCKS = {blocks}\n" + " ".join(extensions))


class ApplicationCompatibilityTests(unittest.TestCase):
    def test_malformed_config_fails_with_actionable_error(self):
        cases = [[], {}, {"schemaVersion": 1, "apps": []},
                 {"schemaVersion": 1, "apps": {"blender": {"executable": "relative"}}},
                 {"schemaVersion": 1, "apps": {"godot": None}}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "apps.json"
            for value in cases:
                with self.subTest(value=value):
                    path.write_text(json.dumps(value), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        app_compat.read_config(path)

    def test_virgl_profile_is_scoped_and_vendor_independent(self):
        inherited = {"DISPLAY": ":1"}
        for vendor in ("NVIDIA", "AMD", "Intel"):
            with self.subTest(vendor=vendor):
                result = app_compat.blender_environment(graphics(f"virgl ({vendor})"), inherited)
                self.assertEqual(result["MESA_GL_VERSION_OVERRIDE"], "4.3")
                self.assertEqual(result["DISPLAY"], ":1")
        self.assertEqual(inherited, {"DISPLAY": ":1"})

    def test_native_capable_graphics_do_not_receive_an_override(self):
        inherited = {"DISPLAY": ":2"}
        self.assertEqual(app_compat.blender_environment(graphics(version="4.6"), inherited), inherited)

    def test_missing_capabilities_and_software_are_rejected(self):
        cases = [graphics(renderer="llvmpipe", version="4.5"),
                 graphics(renderer="Some other hardware"), graphics(blocks=12),
                 graphics(extensions=app_compat.GL43_EXTENSIONS - {"GL_ARB_compute_shader"}),
                 "", graphics(version="3.3")]
        for output in cases:
            with self.subTest(output=output[:80]):
                with self.assertRaises(ValueError):
                    app_compat.blender_environment(output, {})

    @patch("app_compat.os.execvpe")
    @patch("app_compat.os.access", return_value=True)
    @patch("app_compat.subprocess.run")
    def test_blender_virgl_profile_uses_conservative_gpu_paths(self, run, _access, execute):
        run.return_value.stdout = graphics()
        config = {"apps": {"blender": {"executable": "/blender", "virglCompatibility": True}}}
        with patch.dict(os.environ, {"MESA_GL_VERSION_OVERRIDE": "4.6"}):
            app_compat.launch(config, "blender", ["scene.blend"])
        self.assertNotIn("MESA_GL_VERSION_OVERRIDE", run.call_args.kwargs["env"])
        self.assertIn("--debug-gpu-force-workarounds", execute.call_args.args[1])
        self.assertEqual(execute.call_args.args[2]["MESA_GL_VERSION_OVERRIDE"], "4.3")

    @patch("app_compat.os.execvpe")
    @patch("app_compat.os.access", return_value=True)
    def test_godot_arguments_are_forwarded_without_shell_interpretation(self, _access, execute):
        config = {"apps": {"godot": {"executable": "/my apps/godot"}}}
        arguments = ["--path", "/projects/a; echo nope", "--editor"]
        app_compat.launch(config, "godot", arguments)
        self.assertEqual(execute.call_args.args[:2],
                         ("/my apps/godot", ["/my apps/godot", "--rendering-method",
                                             "gl_compatibility", *arguments]))

    @patch("app_compat.os.execvpe")
    @patch("app_compat.os.access", return_value=True)
    @patch("app_compat.subprocess.run")
    def test_blender_background_render_does_not_require_a_desktop(self, run, _access, execute):
        config = {"apps": {"blender": {"executable": "/blender", "virglCompatibility": True}}}
        app_compat.launch(config, "blender", ["--background", "scene.blend"])
        run.assert_not_called()
        execute.assert_called_once()

    def test_desktop_field_codes_are_not_interpreted_in_paths(self):
        self.assertEqual(app_compat.desktop_argument("/home/a b/%f"), '"/home/a b/%%f"')
        with self.assertRaises(ValueError):
            app_compat.desktop_argument("/home/a\nExec=unexpected")

    def test_atomic_write_replaces_owned_file_without_leaving_temporary_files(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "apps.json"
            app_compat.atomic_write(target, "old")
            app_compat.atomic_write(target, "new")
            self.assertEqual(target.read_text(), "new")
            self.assertEqual(list(Path(directory).iterdir()), [target])

    @patch("app_compat.subprocess.run")
    def test_godot_default_preserves_other_preferences_and_original_backup(self, run):
        run.return_value.stdout = "4.7.2.stable.example\n"
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"XDG_CONFIG_HOME": directory}):
            target = Path(directory) / "godot/editor_settings-4.7.tres"
            original = ('[gd_resource type="EditorSettings" format=3]\n\n[resource]\n'
                        'interface/editor/display_scale = 2\n'
                        'project_manager/default_renderer = "forward_plus"\n')
            app_compat.atomic_write(target, original)
            app_compat.godot_default_renderer("godot")
            app_compat.godot_default_renderer("godot")
            self.assertEqual(target.read_text(), original.replace('"forward_plus"', '"gl_compatibility"'))
            self.assertEqual(target.with_suffix(".tres.before-omarchy").read_text(), original)


if __name__ == "__main__":
    unittest.main()
