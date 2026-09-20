#!/usr/bin/env python3
"""Install optional per-user Omarchy application launchers; never edit drivers."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


# Mesa's GL 4.3 requirements, apart from the reserved vertex uniform block.
# A version override cannot add these features: require them before using the
# explicitly selected, patched Blender build on the known VirGL 4.2 path.
GL43_EXTENSIONS = {
    "GL_ARB_ES3_compatibility", "GL_ARB_arrays_of_arrays",
    "GL_ARB_compute_shader", "GL_ARB_copy_image",
    "GL_ARB_explicit_uniform_location", "GL_ARB_fragment_layer_viewport",
    "GL_ARB_framebuffer_no_attachments", "GL_ARB_internalformat_query2",
    "GL_ARB_robust_buffer_access_behavior", "GL_ARB_shader_image_size",
    "GL_ARB_shader_storage_buffer_object", "GL_ARB_stencil_texturing",
    "GL_ARB_texture_buffer_range", "GL_ARB_texture_query_levels",
    "GL_ARB_texture_view",
}


def blender_environment(output, inherited):
    """Narrow compatibility profile, not a claim of GL 4.3 conformance."""
    renderer = re.search(r"^OpenGL renderer string: (.+)$", output, re.M)
    version = re.search(r"^OpenGL core profile version string: (\d+)\.(\d+)", output, re.M)
    if not renderer or not version:
        raise ValueError("Could not identify the OpenGL renderer; run glxinfo -l in the desktop session.")
    if re.search(r"llvmpipe|softpipe|lavapipe|swiftshader", renderer[1], re.I):
        raise ValueError("This launcher requires accelerated graphics; the active renderer is software.")
    env = dict(inherited)
    if tuple(map(int, version.groups())) >= (4, 3):
        return env
    blocks = re.search(r"GL_MAX_VERTEX_UNIFORM_BLOCKS\s*=\s*(\d+)", output)
    extensions = set(re.findall(r"\bGL_[A-Za-z0-9_]+\b", output))
    if (not renderer[1].lower().startswith("virgl") or version.groups() != ("4", "2")
            or not blocks or int(blocks[1]) != 13 or not GL43_EXTENSIONS <= extensions):
        raise ValueError("The renderer does not meet this Blender compatibility profile's requirements.")
    env["MESA_GL_VERSION_OVERRIDE"] = "4.3"
    env["MESA_GLSL_VERSION_OVERRIDE"] = "430"
    return env


def launch(config, application, arguments):
    entry = config["apps"].get(application)
    if entry is None:
        raise ValueError(f"{application} has not been configured.")
    executable = entry["executable"]
    if not os.access(executable, os.X_OK):
        raise ValueError(f"Application is missing or not executable: {executable}")
    env = dict(os.environ)
    flags = []
    if application == "blender":
        flags = ["--gpu-backend", "opengl"]
        if entry.get("virglCompatibility") and not any(a in ("-b", "--background") for a in arguments):
            query_env = dict(env)
            query_env.pop("MESA_GL_VERSION_OVERRIDE", None)
            query_env.pop("MESA_GLSL_VERSION_OVERRIDE", None)
            result = subprocess.run(["glxinfo", "-l"], env=query_env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=10, check=True)
            env = blender_environment(result.stdout, query_env)
            if env.get("MESA_GL_VERSION_OVERRIDE") == "4.3":
                # This tested profile also needs Blender's conservative GPU
                # paths: otherwise Eevee can finish with a corrupt image.
                flags.append("--debug-gpu-force-workarounds")
    elif application == "godot":
        flags = ["--rendering-method", "gl_compatibility"]
    elif application == "supertuxkart":
        flags = ["--render-driver=gl"]
    os.execvpe(executable, [executable, *flags, *arguments], env)


def read_config(path):
    config = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(config, dict) or config.get("schemaVersion") != 1:
        raise ValueError("Unsupported application configuration version.")
    apps = config.get("apps")
    if not isinstance(apps, dict):
        raise ValueError("Application configuration must contain an apps object.")
    for name, entry in apps.items():
        if (name not in ("blender", "godot", "supertuxkart") or not isinstance(entry, dict)
                or not isinstance(entry.get("executable"), str)
                or not Path(entry["executable"]).is_absolute()
                or not isinstance(entry.get("virglCompatibility", False), bool)):
            raise ValueError(f"Invalid application configuration for {name}.")
    return config


def atomic_write(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".omarchy-app-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def desktop_argument(value):
    # Desktop Entry Exec quoting is not shell quoting. Escape reserved characters
    # in quotes, then the backslashes for the enclosing string-value encoding.
    if any(c in value for c in "\r\n\x00"):
        raise ValueError("Desktop entry paths must not contain control characters.")
    quoted = re.sub(r'([\\"`$])', r'\\\1', value.replace("%", "%%"))
    return '"' + quoted.replace("\\", "\\\\") + '"'


def godot_default_renderer(executable):
    version = subprocess.run([executable, "--version"], capture_output=True,
                             text=True, check=True, timeout=10).stdout
    match = re.match(r"(4\.\d+)\.", version)
    if not match:
        raise ValueError("The Godot launcher requires Godot 4.x.")
    root = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "godot"
    settings = root / f"editor_settings-{match[1]}.tres"
    if settings.exists():
        text = settings.read_text(encoding="utf-8")
        if '[resource]' not in text or 'type="EditorSettings"' not in text:
            raise ValueError(f"Unrecognized Godot settings format: {settings}")
    else:
        text = '[gd_resource type="EditorSettings" format=3]\n\n[resource]\n'
    line = 'project_manager/default_renderer = "gl_compatibility"'
    if re.search(r"^project_manager/default_renderer\s*=", text, re.M):
        updated = re.sub(r"^project_manager/default_renderer\s*=.*$", line, text, flags=re.M)
    else:
        updated = text.replace('[resource]', '[resource]\n' + line, 1)
    if updated != text:
        backup = settings.with_suffix(settings.suffix + ".before-omarchy")
        if settings.exists() and not backup.exists():
            atomic_write(backup, text)
        atomic_write(settings, updated)


def install(options):
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    root = data / "try-omarchy-apps"
    config_path = root / "apps.json"
    config = read_config(config_path) if config_path.exists() else {"schemaVersion": 1, "apps": {}}
    selected = {}
    for application in ("blender", "godot", "supertuxkart"):
        value = getattr(options, application)
        if value:
            resolved = shutil.which(value)
            executable = Path(resolved or value).expanduser().resolve(strict=True)
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise ValueError(f"Not an executable file: {executable}")
            # Validate before writing any launcher files.
            desktop_argument(str(executable))
            selected[application] = {"executable": str(executable)}
    if not selected:
        raise ValueError("Select at least one installed application with --blender, --godot or --supertuxkart.")
    if options.blender_virgl_compatibility:
        if "blender" not in selected:
            raise ValueError("--blender-virgl-compatibility requires --blender pointing to the patched build.")
        selected["blender"]["virglCompatibility"] = True
    config["apps"].update(selected)
    launcher = root / "app_compat.py"
    launcher_text = Path(__file__).read_text(encoding="utf-8")
    command = " ".join((desktop_argument(sys.executable), desktop_argument(str(launcher)), "run"))
    names = {"blender": "Blender (Omarchy)", "godot": "Godot (Omarchy)",
             "supertuxkart": "SuperTuxKart (Omarchy)"}
    comments = {"blender": "OpenGL editing and rendering; Cycles uses the CPU",
                "godot": "Godot with the OpenGL Compatibility renderer",
                "supertuxkart": "Race using the OpenGL renderer"}
    if "godot" in selected:
        godot_default_renderer(selected["godot"]["executable"])
    atomic_write(launcher, launcher_text)
    atomic_write(config_path, json.dumps(config, indent=2) + "\n")
    for application in selected:
        entry = data / "applications" / f"try-omarchy-{application}.desktop"
        category = "Game;" if application == "supertuxkart" else "Graphics;3DGraphics;"
        field = " %f" if application == "blender" else ""
        atomic_write(entry, f"[Desktop Entry]\nType=Application\nName={names[application]}\n"
                     f"Comment={comments[application]}\nExec={command} {application}{field}\n"
                     f"Icon={application}\nTerminal=false\nCategories={category}\n")
        print(f"Installed {names[application]}: {entry}")
    print("Existing system launchers, projects and driver settings were retained.")
    if "godot" in selected:
        print("New Godot projects default to Compatibility; previous editor settings were backed up when present.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    setup = sub.add_parser("install", help="Create application entries for the current Linux user")
    for application in ("blender", "godot", "supertuxkart"):
        setup.add_argument("--" + application, metavar="EXECUTABLE")
    setup.add_argument("--blender-virgl-compatibility", action="store_true",
                       help="Opt in to the tested VirGL profile; requires the patched Blender build")
    run = sub.add_parser("run")
    run.add_argument("application", choices=("blender", "godot", "supertuxkart"))
    run.add_argument("arguments", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    try:
        if options.command == "install":
            install(options)
        else:
            config = read_config(Path(__file__).with_name("apps.json"))
            launch(config, options.application, options.arguments)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        message = f"Omarchy application launcher: {error}"
        print(message, file=sys.stderr)
        if shutil.which("notify-send"):
            try:
                subprocess.run(["notify-send", "Omarchy", message], timeout=5, check=False)
            except (OSError, subprocess.SubprocessError):
                pass  # Keep the original launch error if desktop notifications fail.
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
