"""Verify a compile-only Linux Mesa staging tree by reading files, never loading it."""
import argparse
import hashlib
import json
from pathlib import Path
import os
from elftools.elf.elffile import ELFFile


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def check(condition, message):
    if not condition:
        raise ValueError(message)


def inspect_elf(path, include_exports=False):
    with path.open('rb') as f:
        elf = ELFFile(f)
        check(elf.elfclass == 64 and elf.little_endian and
              elf['e_machine'] == 'EM_X86_64' and elf['e_type'] == 'ET_DYN',
              'Expected little-endian x86-64 ELF shared library: ' + path.name)
        definitions = elf.get_section_by_name('.gnu.version_d')
        versions = {}
        if definitions:
            for version, auxiliary in definitions.iter_versions():
                versions[version['vd_ndx']] = next(auxiliary).name
        version_table = elf.get_section_by_name('.gnu.version')
        exports, version_nodes = {}, []
        symbols = elf.get_section_by_name('.dynsym')
        check(symbols is not None, 'Dynamic symbol table missing')
        for index, symbol in enumerate(symbols.iter_symbols()):
            if not symbol.name or symbol['st_shndx'] == 'SHN_UNDEF' or symbol['st_other']['visibility'] not in ['STV_DEFAULT', 'STV_PROTECTED']:
                continue
            binding = symbol['st_info']['bind']
            if binding == 'STB_LOCAL':
                continue
            check(binding in ['STB_GLOBAL', 'STB_WEAK'], 'Unexpected exported symbol binding')
            if (symbol.name in versions.values() and symbol['st_shndx'] == 'SHN_ABS' and
                    symbol['st_value'] == 0 and symbol['st_size'] == 0 and
                    symbol['st_info']['type'] == 'STT_OBJECT'):
                version_nodes.append(symbol.name)
                continue
            raw = version_table.get_symbol(index)['ndx'] if version_table else 'VER_NDX_GLOBAL'
            number = raw & 0x7fff if isinstance(raw, int) else 1
            exports[symbol.name] = dict(type=symbol['st_info']['type'], binding=binding,
                                       visibility=symbol['st_other']['visibility'],
                                       version=versions[number] if number > 1 else None,
                                       nonDefault=bool(raw & 0x8000) if isinstance(raw, int) else False)
        soname, needed, runpaths = [], [], []
        dynamic = elf.get_section_by_name('.dynamic')
        check(dynamic is not None, 'Dynamic section missing')
        for tag in dynamic.iter_tags():
            if tag.entry.d_tag == 'DT_SONAME':
                soname.append(tag.soname)
            elif tag.entry.d_tag == 'DT_NEEDED':
                needed.append(tag.needed)
            elif tag.entry.d_tag == 'DT_RUNPATH':
                runpaths.append(tag.runpath)
            elif tag.entry.d_tag == 'DT_RPATH':
                runpaths.append(tag.rpath)
        encoded = json.dumps(exports, sort_keys=True, separators=(',', ':')).encode()
        result = dict(sha256=digest(path), bytes=path.stat().st_size, soname=soname,
                      neededLibraries=needed, runpaths=runpaths, apiExportCount=len(exports),
                      apiExportAbiSha256=hashlib.sha256(encoded).hexdigest(),
                      syntheticAbsoluteVersionNodes=version_nodes)
        if include_exports:
            result['apiExports'] = exports
        return result


def verify_aliases(stage, aliases):
    stage = stage.resolve(strict=True)
    links = {}
    for path in stage.rglob('*'):
        if path.is_symlink():
            target = path.resolve(strict=True)
            check(target.is_relative_to(stage), 'Staged symlink escapes destination')
            links[path.relative_to(stage).as_posix()] = os.readlink(path)
    for relative, expected in aliases.items():
        path = stage / relative
        check(path.is_symlink(), 'Required installed library alias missing: ' + relative)
        expected_target = (stage / expected).resolve(strict=True)
        check(expected_target.is_relative_to(stage), 'Library alias contract escapes destination')
        check(path.resolve(strict=True) == expected_target,
              'Installed library alias points to the wrong library: ' + relative)
    return links


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['source', 'build', 'stage', 'output']:
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--recipe', type=Path,
                        help='Versioned recipe; omitted keeps the historical 26.0.8 contract')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source, build, stage = [getattr(args, n).resolve(strict=True) for n in ['source', 'build', 'stage']]
    recipe = args.recipe.resolve(strict=True) if args.recipe else here
    contract = json.loads((recipe / 'stage-contract.json').read_text(encoding='utf-8'))
    options = {r['name']: r['value'] for r in json.loads((build / 'meson-info/intro-buildoptions.json').read_text(encoding='utf-8'))}
    check(options['prefix'] == '/usr' and options['libdir'] == 'lib', 'Unexpected installation prefix')
    check(options['gallium-drivers'] == ['d3d12'] and 'wayland' in options['platforms'], 'Wrong desktop build profile')
    check(options['build-tests'] is False and options['vulkan-drivers'] == [] and options['gallium-d3d12-video'] == 'disabled', 'Unexpected graphics test or API profile')
    check((source / 'VERSION').read_text(encoding='utf-8').strip() == contract['mesaVersion'], 'Wrong Mesa source version')
    if args.recipe:
        from apply_mesa_recipe import verify_patched
        manifest, sources = verify_patched(source, recipe, 'graphics')
        check(manifest['mesaVersion'] == contract['mesaVersion'], 'Recipe/contract version differs')
    else:
        expected = {}
        for name in ['fence-fd-patch.json', 'native-sync-patch.json', 'native-image-patch.json',
                     'native-render-patch.json', 'native-presentation-patch.json',
                     'upload-failure-patch.json', 'constbuf-failure-patch.json']:
            manifest = json.loads((here / name).read_text(encoding='utf-8'))
            expected.update({n: r['patchedSha256'] for n, r in manifest['files'].items()})
        sources = {}
        for relative, wanted in expected.items():
            path = (source / relative).resolve(strict=True)
            check(path.is_relative_to(source), 'Source path escapes supplied tree')
            actual = digest(path)
            check(actual == wanted, 'Patched source hash differs: ' + relative)
            sources[relative] = actual
    generated = {}
    for relative, wanted in contract['generatedWaylandFiles'].items():
        actual = digest(build / relative)
        check(actual == wanted, 'Generated Wayland source differs: ' + relative)
        generated[relative] = actual
    dependencies = {r['name']: r.get('version') for r in json.loads((build / 'meson-info/intro-dependencies.json').read_text(encoding='utf-8'))}
    required_dependencies = {'wayland-client': '1.26.0', 'wayland-server': '1.26.0',
                             'wayland-egl-backend': '3', 'wayland-protocols': '1.47'}
    check(all(dependencies.get(n) == v for n, v in required_dependencies.items()),
          'Required pinned Wayland dependency versions differ')
    ninja = (build / 'build.ninja').read_text(encoding='utf-8')
    for flag in ['HAVE_WL_DISPATCH_QUEUE_TIMEOUT', 'HAVE_WL_CREATE_QUEUE_WITH_NAME']:
        check('-D' + flag in ninja, 'Required Wayland code path not compiled: ' + flag)
    links = verify_aliases(stage, contract['libraryAliases'])
    for relative in contract['requiredFiles']:
        check((stage / relative).is_file(), 'Required staged development file missing: ' + relative)
    observations = {}
    if args.recipe:
        # Preserve actual ABI metadata even when the strict historical baseline
        # rejects a new release. This report is evidence for review, not an
        # accepted component or an instruction to update expected hashes.
        for relative in contract['libraries']:
            path = (stage / relative).resolve(strict=True)
            check(path.is_relative_to(stage), 'Library escapes staging root')
            observations[relative] = inspect_elf(path, include_exports=True)
        report = dict(schema=1, headCommit=os.environ.get('SOURCE_HEAD_COMMIT'),
                      sourceCommit=os.environ.get('SOURCE_COMMIT'),
                      mesaVersion=contract['mesaVersion'],
                      baselineMesaVersion=contract['abiComparisonBaselineMesaVersion'],
                      stageContractSha256=digest(recipe / 'stage-contract.json'),
                      recipeManifestSha256=digest(recipe / 'recipe.json'),
                      observedLibraries=observations, expectedLibraries=contract['libraries'],
                      inspectionOnly=True, binaryInterfaceComparisonPassed=False,
                      componentAccepted=False, runtimeLoaded=False,
                      gpuWorkloadRun=False, vmLaunched=False, dailyUseAccepted=False)
        args.output.with_name(args.output.stem + '-abi-observations.json').write_text(
            json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    libraries = {}
    for relative, wanted in contract['libraries'].items():
        path = (stage / relative).resolve(strict=True)
        check(path.is_relative_to(stage), 'Library escapes staging root')
        actual = ({k: v for k, v in observations[relative].items() if k != 'apiExports'}
                  if observations else inspect_elf(path))
        check(actual['soname'] == wanted['soname'], 'SONAME differs: ' + relative)
        check(not actual['runpaths'], 'Runtime build search path retained: ' + relative)
        check(all('/' not in name and '\\' not in name and ':' not in name for name in actual['neededLibraries']),
              'Dependency contains an absolute or host path')
        if 'neededLibraries' in wanted:
            check(actual['neededLibraries'] == wanted['neededLibraries'],
                  'Reviewed component dependencies differ: ' + relative)
        check(actual['apiExportCount'] == wanted['apiExportCount'] and
              actual['apiExportAbiSha256'] == wanted['apiExportAbiSha256'],
              'API exports, types, bindings, visibility or versions differ: ' + relative)
        libraries[relative] = actual
    result = dict(schema=2, sourceCommit=os.environ.get('SOURCE_COMMIT'),
                  headCommit=os.environ.get('SOURCE_HEAD_COMMIT'), mesaVersion=contract['mesaVersion'],
                  installPrefix='/usr', libdir='lib', patchedSourceSha256=sources,
                  independentlyGeneratedWaylandSha256=generated, stagedLibraries=libraries,
                  stagedLibraryAliases=links, verifiedLibraryAliasTargets=contract['libraryAliases'],
                  comparedApiExports=sum(r['apiExportCount'] for r in libraries.values()),
                  waylandDependencyVersions=required_dependencies, libraryLayoutVerified=True,
                  binaryInterfaceComparisonPassed=True, standaloneRuntimePackage=False,
                  runtimeLoaded=False, gpuWorkloadRun=False, vmLaunched=False,
                  hardwareIntegrationTested=False, quickshellCrashResolutionVerified=False,
                  performanceAcceptanceVerified=False, dailyUseAccepted=False)
    if args.recipe:
        result.update(recipeManifestSha256=digest(recipe / 'recipe.json'),
                      abiComparisonBaselineMesaVersion=contract['abiComparisonBaselineMesaVersion'])
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
