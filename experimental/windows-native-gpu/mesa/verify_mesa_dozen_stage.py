"""Inspect a staged Dozen ICD as data; never load a library or enumerate devices."""
import argparse
import json
import os
from pathlib import Path
import re

from elftools.elf.elffile import ELFFile
from verify_mesa_stage import check, digest, inspect_elf, verify_aliases


LIBRARY = 'usr/lib/libvulkan_dzn.so'
ICD = 'usr/share/vulkan/icd.d/dzn_icd.x86_64.json'
MANIFESTS = (
    ('dozen-memory-fd-patch.json', '0005-linux-dozen-memory-fds.patch'),
    ('dozen-memory-fd-import-patch.json', '0006-linux-dozen-import-memory-fds.patch'),
    ('dozen-native-gpu-buffer-patch.json', '0007-linux-dozen-native-gpu-buffers.patch'),
    ('dozen-buffer-admission-patch.json', '0008-linux-dozen-buffer-admission-errors.patch'),
)


def inside(root, relative):
    path = (root / relative).resolve(strict=True)
    check(path.is_relative_to(root), 'File escapes supplied root: ' + relative)
    check(path.is_file(), 'Expected regular file: ' + relative)
    return path


def verify_icd(stage, api_version):
    stage = stage.resolve(strict=True)
    links = verify_aliases(stage, {})
    library = inside(stage, LIBRARY)
    check(not (stage / LIBRARY).is_symlink(), 'ICD library must be a regular file')
    manifest = inside(stage, ICD)
    document = json.loads(manifest.read_text(encoding='utf-8'))
    check(isinstance(document, dict) and document.get('file_format_version') == '1.0.1',
          'Unexpected ICD manifest format')
    record = document.get('ICD')
    check(isinstance(record, dict), 'ICD manifest record missing')
    check(record.get('library_path') == '/usr/lib/libvulkan_dzn.so',
          'ICD manifest does not select the installed Dozen library')
    check(record.get('api_version') == api_version, 'ICD API version differs from pinned source')
    check(record.get('library_arch') == '64', 'ICD architecture is not 64-bit')
    actual = inspect_elf(library)
    check(actual['soname'] == ['libvulkan_dzn.so'], 'Unexpected Dozen SONAME')
    check(not actual['runpaths'], 'Dozen retains a build search path')
    check(all('/' not in name and '\\' not in name and ':' not in name
              for name in actual['neededLibraries']), 'Dozen dependency contains a host path')
    with library.open('rb') as stream:
        symbols = ELFFile(stream).get_section_by_name('.dynsym')
        candidates = symbols.get_symbol_by_name('vk_icdGetInstanceProcAddr') or []
        check(any(s['st_shndx'] != 'SHN_UNDEF' and
                  s['st_info']['type'] == 'STT_FUNC' and
                  s['st_info']['bind'] == 'STB_GLOBAL' and
                  s['st_other']['visibility'] == 'STV_DEFAULT' for s in candidates),
              'Dozen loader entry point is not an exported function')
    return dict(stagedLibraries={LIBRARY: actual}, stagedLibraryAliases=links,
                icdManifest=dict(path=ICD, sha256=digest(manifest), contents=document))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'stage', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--recipe', type=Path,
                        help='Versioned recipe; omitted preserves the 26.0.8 contract')
    args = parser.parse_args()
    source, build, stage = [getattr(args, n).resolve(strict=True)
                            for n in ('source', 'build', 'stage')]
    here = Path(__file__).resolve().parent
    version = (source / 'VERSION').read_text(encoding='utf-8').strip()
    recipe = args.recipe.resolve(strict=True) if args.recipe else None
    mesa_version = json.loads((recipe / 'recipe.json').read_text(encoding='utf-8'))['mesaVersion'] if recipe else '26.0.8'
    check(version == mesa_version,
          'Wrong Mesa source version')
    options = {r['name']: r['value'] for r in json.loads(
        (build / 'meson-info/intro-buildoptions.json').read_text(encoding='utf-8'))}
    required = dict(prefix='/usr', libdir='lib', datadir='share',
                    platforms=['wayland'], **{'gallium-drivers': [],
                    'vulkan-drivers': ['microsoft-experimental'],
                    'vulkan-manifest-per-architecture': True,
                    'build-tests': False, 'opengl': False, 'llvm': 'disabled',
                    'egl': 'disabled', 'gbm': 'disabled', 'glx': 'disabled',
                    'gles1': 'disabled', 'gles2': 'disabled',
                    'gallium-d3d12-video': 'disabled'})
    for name, expected in required.items():
        check(options.get(name) == expected, 'Unexpected Dozen build option: ' + name)
    expected_sources, patches = {}, {}
    if recipe:
        from apply_mesa_recipe import verify_patched
        manifest, sources = verify_patched(source, recipe, 'dozen')
        profile = manifest['profiles']['dozen']
        patches[profile['patchFile']] = profile['patchSha256']
    else:
        for manifest_name, patch_name in MANIFESTS:
            manifest = json.loads((here / manifest_name).read_text(encoding='utf-8'))
            actual = digest(here / patch_name)
            check(actual == manifest['patchSha256'], 'Dozen recipe patch hash differs')
            patches[patch_name] = actual
            expected_sources.update({n: r['patchedSha256'] for n, r in manifest['files'].items()})
        sources = {}
        for relative, expected in expected_sources.items():
            actual = digest(inside(source, relative))
            check(actual == expected, 'Patched Dozen source differs: ' + relative)
            sources[relative] = actual
    header = (source / 'include/vulkan/vulkan_core.h').read_text(encoding='utf-8')
    version = re.search(r'^#define\s+VK_HEADER_VERSION\s+(\d+)\s*$', header, re.MULTILINE)
    check(version is not None, 'Pinned Vulkan header version missing')
    if recipe:
        # Mesa intentionally advertises a conservative loader API version.
        # Do not replace it with DZN_API_VERSION or a conformance claim.
        meson_source = (source / 'src/microsoft/vulkan/meson.build').read_text(encoding='utf-8')
        versions = set(re.findall(r"'--api-version',\s*'(\d+\.\d+)'", meson_source))
        check(len(versions) == 1, 'Pinned Dozen ICD API generation version is ambiguous')
        api_prefix = versions.pop() + '.'
    else:
        api_prefix = '1.1.'
    api_version = api_prefix + version.group(1)
    icd = verify_icd(stage, api_version)
    dependencies = {r['name']: r.get('version') for r in json.loads(
        (build / 'meson-info/intro-dependencies.json').read_text(encoding='utf-8'))}
    for name, version in {'wayland-client': '1.26.0', 'wayland-protocols': '1.47'}.items():
        check(dependencies.get(name) == version, 'Pinned Wayland dependency differs: ' + name)
    files = {p.relative_to(stage).as_posix(): digest(inside(stage, p.relative_to(stage).as_posix()))
             for p in stage.rglob('*') if p.is_file() and not p.is_symlink()}
    result = dict(schema=1, sourceCommit=os.environ.get('SOURCE_COMMIT'),
                  headCommit=os.environ.get('SOURCE_HEAD_COMMIT'), mesaVersion=mesa_version,
                  buildOptions=required, patchSha256=patches, patchedSourceSha256=sources,
                  wsiSourceSha256=digest(inside(source, 'src/microsoft/vulkan/dzn_wsi.c')),
                  waylandDependencyVersions={n: dependencies[n] for n in
                                             ('wayland-client', 'wayland-protocols')},
                  stagedFilesSha256=files, **icd,
                  libraryLayoutVerified=True, loaderMetadataVerified=True,
                  runtimeLoaded=False, gpuWorkloadRun=False, vmLaunched=False,
                  waylandGpuPresentationImplemented=False, apiConformanceVerified=False,
                  driverOrDisplaySafetyVerified=False, performanceAcceptanceVerified=False,
                  standaloneRuntimePackage=False, dailyUseAccepted=False)
    if recipe:
        result['recipeManifestSha256'] = digest(recipe / 'recipe.json')
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
