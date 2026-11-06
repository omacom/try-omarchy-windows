#!/usr/bin/env bash
# CPU-only compilation on GitHub-hosted Linux; no ICD/library/device activation.
set -euo pipefail
if [[ "${GITHUB_ACTIONS:-}" != true || "${RUNNER_OS:-}" != Linux ]]; then
  printf '%s\n' 'Requires the Linux GitHub Actions build environment.' >&2
  exit 2
fi
: "${RUNNER_TEMP:?}" "${GITHUB_WORKSPACE:?}" "${MESA_PROFILE:?}"
case "$MESA_PROFILE" in graphics|dozen) ;; *) exit 2 ;; esac
cd "$GITHUB_WORKSPACE"
here=experimental/windows-native-gpu/mesa
recipe="$here/recipes/26.2.6"
source_dir="$RUNNER_TEMP/mesa-26.2.6"
build_dir="$RUNNER_TEMP/mesa-$MESA_PROFILE-build"
stage_dir="$RUNNER_TEMP/mesa-$MESA_PROFILE-stage"
component_dir="$RUNNER_TEMP/mesa-$MESA_PROFILE-component"
receipt="$RUNNER_TEMP/mesa-$MESA_PROFILE-source-build.json"
meson="$RUNNER_TEMP/meson-env/bin/meson"
python="$RUNNER_TEMP/meson-env/bin/python"

"$python" "$here/test_mesa_recipe.py" --source "$source_dir" --recipe "$recipe" \
  --profile "$MESA_PROFILE" --output "$RUNNER_TEMP/mesa-$MESA_PROFILE-recipe-controls"
"$python" "$here/apply_mesa_recipe.py" "$source_dir" --recipe "$recipe" --profile "$MESA_PROFILE"
export PATH="$RUNNER_TEMP/wayland-install/bin:$PATH"
export PKG_CONFIG_PATH="$RUNNER_TEMP/headers-install/lib/pkgconfig:$RUNNER_TEMP/wayland-install/lib/pkgconfig:$RUNNER_TEMP/protocols-install/share/pkgconfig"
test "$(pkg-config --modversion DirectX-Headers)" = '1.619.5'
test "$(pkg-config --modversion wayland-protocols)" = '1.47'
common=(--prefix /usr --libdir lib --datadir share --wrap-mode=nofallback --buildtype=release
  -Dplatforms=wayland -Dgles1=disabled -Dglx=disabled -Dglvnd=disabled
  -Dllvm=disabled -Dxmlconfig=disabled -Dvalgrind=disabled
  -Dgallium-d3d12-video=disabled -Dgallium-va=disabled -Dvideo-codecs=[] -Dbuild-tests=false)
if [[ "$MESA_PROFILE" == graphics ]]; then
  "$meson" setup "$build_dir" "$source_dir" "${common[@]}" \
    -Dgallium-drivers=d3d12 -Dvulkan-drivers=[] -Dgles2=enabled -Degl=enabled -Dgbm=enabled
else
  "$meson" setup "$build_dir" "$source_dir" "${common[@]}" \
    -Dgallium-drivers=[] -Dvulkan-drivers=microsoft-experimental \
    -Dvulkan-manifest-per-architecture=true -Dopengl=false -Dgles2=disabled -Degl=disabled -Dgbm=disabled
fi
"$meson" compile -C "$build_dir" -j 2
"$meson" install -C "$build_dir" --no-rebuild --destdir "$stage_dir"
if [[ "$MESA_PROFILE" == graphics ]]; then
  "$python" "$here/test_mesa_stage_aliases.py" --stage "$stage_dir" --recipe "$recipe" \
    --output "$RUNNER_TEMP/mesa-graphics-alias-controls"
  "$python" "$here/verify_mesa_stage.py" --source "$source_dir" --recipe "$recipe" \
    --build "$build_dir" --stage "$stage_dir" --output "$receipt"
else
  "$python" "$here/verify_mesa_dozen_stage.py" --source "$source_dir" --recipe "$recipe" \
    --build "$build_dir" --stage "$stage_dir" --output "$receipt"
  "$python" "$here/test_mesa_dozen_stage.py" --stage "$stage_dir" \
    --output "$RUNNER_TEMP/mesa-dozen-layout-controls.json"
fi

mkdir -p "$component_dir/root" "$component_dir/licenses"
cp -a "$stage_dir/usr" "$component_dir/root/"
cp "$receipt" "$component_dir/BUILD-RECEIPT.json"
cp "$recipe/README.md" "$component_dir/README.md"
cp "$recipe/recipe.json" "$component_dir/SOURCE-RECIPE.json"
cp -a "$source_dir/licenses" "$component_dir/licenses/Mesa"
cp "$source_dir/docs/license.rst" "$component_dir/licenses/Mesa-license.rst"
cp .runtime-headers/LICENSE "$component_dir/licenses/DirectX-Headers-LICENSE"
archive="$RUNNER_TEMP/mesa-26.2.6-$MESA_PROFILE-components.tar.gz"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
  -C "$component_dir" -cf - . | gzip -n > "$archive"
"$python" - "$archive" <<'PY'
import hashlib, json, pathlib, sys, tarfile
path = pathlib.Path(sys.argv[1])
with tarfile.open(path, 'r:gz') as archive:
    entries = archive.getmembers()
    members = {m.name.removeprefix('./'): m for m in entries}
    if len(members) != len(entries):
        raise RuntimeError('Duplicate component archive member')
    receipt = json.load(archive.extractfile(members['BUILD-RECEIPT.json']))
    files = receipt.get('stagedFilesSha256') or {n: r['sha256'] for n, r in receipt['stagedLibraries'].items()}
    for relative, expected in files.items():
        member = members['root/' + relative]
        if not member.isfile() or hashlib.sha256(archive.extractfile(member).read()).hexdigest() != expected:
            raise RuntimeError('Component archive file differs: ' + relative)
    for relative, target in receipt['stagedLibraryAliases'].items():
        member = members['root/' + relative]
        if not member.issym() or member.linkname != target:
            raise RuntimeError('Component archive alias differs: ' + relative)
print('MESA_26_2_6_COMPONENT_ARCHIVE_PASS runtimeLoaded=false')
PY
cd "$RUNNER_TEMP"
sha256sum "$(basename "$archive")" > "$archive.sha256"
