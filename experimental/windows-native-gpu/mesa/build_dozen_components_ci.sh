#!/usr/bin/env bash
# GitHub-hosted CPU build and file inspection only. Never load the resulting ICD.
set -euo pipefail
if [[ "${GITHUB_ACTIONS:-}" != true || "${RUNNER_OS:-}" != Linux ]]; then
  printf '%s\n' 'This entry point requires the Linux GitHub Actions build environment.' >&2
  exit 2
fi
: "${RUNNER_TEMP:?}" "${GITHUB_WORKSPACE:?}"
cd "$GITHUB_WORKSPACE"
recipe=experimental/windows-native-gpu/mesa
source_dir="$RUNNER_TEMP/mesa-26.0.8"
build_dir="$RUNNER_TEMP/mesa-dozen-build"
stage_dir="$RUNNER_TEMP/mesa-dozen-stage"
component_dir="$RUNNER_TEMP/mesa-dozen-component"
meson="$RUNNER_TEMP/meson-env/bin/meson"
python="$RUNNER_TEMP/meson-env/bin/python"

# Earlier workflow steps verify the source archive and build the pinned headers
# and Wayland scanner. Dozen changes separate source files from the GL guards.
"$python" "$recipe/apply_mesa_dozen_memory_fd.py" "$source_dir"
"$python" "$recipe/apply_mesa_dozen_memory_fd_import.py" "$source_dir"
"$python" "$recipe/apply_mesa_dozen_native_gpu_buffer.py" "$source_dir"
"$python" "$recipe/apply_mesa_dozen_buffer_admission.py" "$source_dir"
export PATH="$RUNNER_TEMP/wayland-install/bin:$PATH"
export PKG_CONFIG_PATH="$RUNNER_TEMP/headers-install/lib/pkgconfig:$RUNNER_TEMP/wayland-install/lib/pkgconfig:$RUNNER_TEMP/protocols-install/share/pkgconfig"
"$meson" setup "$build_dir" "$source_dir" --prefix /usr --libdir lib --datadir share \
  --wrap-mode=nofallback --buildtype=release -Dplatforms=wayland \
  -Dvulkan-drivers=microsoft-experimental -Dvulkan-manifest-per-architecture=true \
  -Dgallium-drivers=[] -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dglvnd=disabled \
  -Dllvm=disabled -Dxmlconfig=disabled -Dvalgrind=disabled \
  -Dgallium-d3d12-video=disabled -Dgallium-va=disabled -Dvideo-codecs=[] -Dbuild-tests=false
"$meson" compile -C "$build_dir" -j 2
"$meson" install -C "$build_dir" --no-rebuild --destdir "$stage_dir"
"$python" "$recipe/verify_mesa_dozen_stage.py" --source "$source_dir" \
  --build "$build_dir" --stage "$stage_dir" --output "$RUNNER_TEMP/mesa-dozen-source-build.json"
"$python" "$recipe/test_mesa_dozen_stage.py" --stage "$stage_dir" \
  --output "$RUNNER_TEMP/mesa-dozen-controls.json"

mkdir -p "$component_dir/root" "$component_dir/licenses"
cp -a "$stage_dir/usr" "$component_dir/root/"
cp "$RUNNER_TEMP/mesa-dozen-source-build.json" "$component_dir/BUILD-RECEIPT.json"
cp "$recipe/DOZEN-COMPONENT-README.md" "$component_dir/README.md"
cp -a "$source_dir/licenses" "$component_dir/licenses/Mesa"
cp "$source_dir/docs/license.rst" "$component_dir/licenses/Mesa-license.rst"
cp .runtime-headers/LICENSE "$component_dir/licenses/DirectX-Headers-LICENSE"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
  -C "$component_dir" -cf - . | gzip -n > "$RUNNER_TEMP/mesa-dozen-wayland-components.tar.gz"
"$python" - <<'PY'
import hashlib, json, os, pathlib, tarfile
path = pathlib.Path(os.environ['RUNNER_TEMP']) / 'mesa-dozen-wayland-components.tar.gz'
with tarfile.open(path, 'r:gz') as archive:
    members = {m.name.removeprefix('./'): m for m in archive.getmembers()}
    receipt = json.load(archive.extractfile(members['BUILD-RECEIPT.json']))
    for relative, expected in receipt['stagedFilesSha256'].items():
        member = members['root/' + relative]
        if not member.isfile() or hashlib.sha256(archive.extractfile(member).read()).hexdigest() != expected:
            raise RuntimeError('Archived Dozen component file differs: ' + relative)
    for relative, target in receipt['stagedLibraryAliases'].items():
        member = members['root/' + relative]
        if not member.issym() or member.linkname != target:
            raise RuntimeError('Archived Dozen component alias differs: ' + relative)
print('DOZEN_COMPONENT_ARCHIVE_FILES_PASS runtimeLoaded=false')
PY
cd "$RUNNER_TEMP"
sha256sum mesa-dozen-wayland-components.tar.gz > mesa-dozen-wayland-components.tar.gz.sha256
