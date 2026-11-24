"""Data-only rejection controls using a compiled ICD; no Vulkan API is called."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile

from elftools.common.exceptions import ELFError
from verify_mesa_dozen_stage import ICD, LIBRARY, verify_icd
from verify_mesa_stage import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    stage = args.stage.resolve(strict=True)
    source_library = stage / LIBRARY
    source_digest = digest(source_library)
    original = json.loads((stage / ICD).read_text(encoding='utf-8'))
    api_version = original['ICD']['api_version']
    verify_icd(stage, api_version)
    cases = [{'case': 'valid-compiled-icd', 'accepted': True}]
    controls = ('manifest-host-path', 'manifest-relative-path', 'manifest-api',
                'manifest-architecture', 'manifest-format', 'missing-library',
                'not-elf', 'escaping-library', 'wrong-soname', 'missing-loader-function')
    for name in controls:
        with tempfile.TemporaryDirectory(prefix='mesa-dozen-control-') as directory:
            fixture = Path(directory)
            library, manifest = fixture / LIBRARY, fixture / ICD
            library.parent.mkdir(parents=True)
            manifest.parent.mkdir(parents=True)
            shutil.copy2(source_library, library)
            document = json.loads(json.dumps(original))
            if name == 'manifest-host-path':
                document['ICD']['library_path'] = '/tmp/libvulkan_dzn.so'
            elif name == 'manifest-relative-path':
                document['ICD']['library_path'] = 'libvulkan_dzn.so'
            elif name == 'manifest-api':
                document['ICD']['api_version'] = '9.9.999'
            elif name == 'manifest-architecture':
                document['ICD']['library_arch'] = '32'
            elif name == 'manifest-format':
                document['file_format_version'] = '9.9.9'
            elif name == 'missing-library':
                library.unlink()
            elif name == 'not-elf':
                library.write_bytes(b'not an ELF shared library\n')
            elif name == 'escaping-library':
                library.unlink()
                library.symlink_to(source_library)
            else:
                old, new = ((b'libvulkan_dzn.so', b'libvulkan_xxx.so') if name == 'wrong-soname'
                            else (b'vk_icdGetInstanceProcAddr', b'xx_icdGetInstanceProcAddr'))
                data = library.read_bytes()
                if len(old) != len(new) or old not in data:
                    raise RuntimeError('Cannot construct binary metadata control: ' + name)
                library.write_bytes(data.replace(old, new))
            manifest.write_text(json.dumps(document), encoding='utf-8', newline='\n')
            try:
                verify_icd(fixture, api_version)
            except (ValueError, OSError, ELFError):
                cases.append({'case': name, 'accepted': False})
            else:
                raise RuntimeError('Invalid ICD accepted: ' + name)
    if digest(source_library) != source_digest:
        raise RuntimeError('Controls changed the original compiled ICD')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(schema=1, cases=cases, passed=True,
        runtimeLoaded=False, gpuWorkloadRun=False, vmLaunched=False), indent=2) + '\n',
        encoding='utf-8', newline='\n')
    print(f'DOZEN_STAGE_CONTROLS_PASS cases={len(cases)} runtimeLoaded=false')


if __name__ == '__main__':
    main()
