"""Static ELF metadata for private guest candidates; never load an ELF library."""
import hashlib
import io
import json
from collections import deque

from elftools.elf.elffile import ELFFile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect(data, label):
    require(len(data) <= 128 * 1024 * 1024, 'ELF exceeds inspection bound: ' + label)
    elf = ELFFile(io.BytesIO(data))
    require(elf.elfclass == 64 and elf.little_endian and
            elf['e_machine'] == 'EM_X86_64' and elf['e_type'] == 'ET_DYN',
            'Expected x86-64 shared ELF: ' + label)
    defined_versions, required_versions = {}, {}
    section = elf.get_section_by_name('.gnu.version_d')
    if section:
        for version, auxiliary in section.iter_versions():
            defined_versions[version['vd_ndx']] = next(auxiliary).name
    section = elf.get_section_by_name('.gnu.version_r')
    if section:
        for version, auxiliary in section.iter_versions():
            for item in auxiliary:
                required_versions[item['vna_other'] & 0x7fff] = (version.name, item.name)
    table = elf.get_section_by_name('.gnu.version')
    symbols = elf.get_section_by_name('.dynsym')
    require(symbols is not None, 'Missing dynamic symbols: ' + label)
    require(symbols.num_symbols() <= 200000, 'Dynamic symbol count exceeds bound: ' + label)
    exports, imports, abi = [], [], {}
    for index, symbol in enumerate(symbols.iter_symbols()):
        name, binding = symbol.name, symbol['st_info']['bind']
        if not name or binding == 'STB_LOCAL':
            continue
        require(binding in ('STB_GLOBAL', 'STB_WEAK', 'STB_LOOS'), 'Unsupported ELF binding: ' + label)
        raw = table.get_symbol(index)['ndx'] if table else 'VER_NDX_GLOBAL'
        number = raw & 0x7fff if isinstance(raw, int) else 1
        if symbol['st_shndx'] == 'SHN_UNDEF':
            require(number <= 1 or number in required_versions, 'Missing import version: ' + label)
            library, version = required_versions.get(number, (None, None))
            imports.append(dict(name=name, type=symbol['st_info']['type'], weak=binding == 'STB_WEAK',
                                library=library, version=version))
            continue
        if symbol['st_other']['visibility'] not in ('STV_DEFAULT', 'STV_PROTECTED'):
            continue
        if (name in defined_versions.values() and symbol['st_shndx'] == 'SHN_ABS' and
                not symbol['st_value'] and not symbol['st_size'] and symbol['st_info']['type'] == 'STT_OBJECT'):
            continue
        require(number <= 1 or number in defined_versions, 'Missing export version: ' + label)
        record = dict(type=symbol['st_info']['type'], binding=binding,
                      visibility=symbol['st_other']['visibility'],
                      version=defined_versions[number] if number > 1 else None,
                      nonDefault=bool(raw & 0x8000) if isinstance(raw, int) else False)
        exports.append(dict(name=name, **record))
        # Matches the existing component stage receipt's canonical API map.
        abi[name] = record
    dynamic = elf.get_section_by_name('.dynamic')
    require(dynamic is not None, 'Missing dynamic section: ' + label)
    needed, soname, runpaths = [], [], []
    for tag in dynamic.iter_tags():
        if tag.entry.d_tag == 'DT_NEEDED':
            needed.append(tag.needed)
        elif tag.entry.d_tag == 'DT_SONAME':
            soname.append(tag.soname)
        elif tag.entry.d_tag == 'DT_RUNPATH':
            runpaths.append(tag.runpath)
        elif tag.entry.d_tag == 'DT_RPATH':
            runpaths.append(tag.rpath)
    require(all(name and '/' not in name and '\\' not in name and ':' not in name for name in needed),
            'Dependency contains a path: ' + label)
    require(len(needed) <= 128, 'Too many ELF dependencies: ' + label)
    return dict(sha256=hashlib.sha256(data).hexdigest(), needed=needed, soname=soname,
                runpaths=runpaths, exports=exports, imports=imports,
                definedVersionNames=sorted(set(defined_versions.values())),
                requiredVersionNodes=[dict(library=n, version=v) for n, v in sorted(set(required_versions.values()))],
                apiExportCount=len(abi),
                apiExportAbiSha256=hashlib.sha256(json.dumps(abi, sort_keys=True,
                    separators=(',', ':')).encode()).hexdigest())


def dependency_preflight(roots, blobs, search_roots):
    """Check declared DT_NEEDED closure and strong imports in captured files.

    Search order is explicit; no host files, ld.so cache, dlopen or preloads are
    consulted. This is admission metadata, not an implementation of ld.so.
    """
    records, by_needed, queue = {}, {}, deque(roots)
    while queue:
        path = queue.popleft()
        if path in records:
            continue
        require(path in blobs, 'Captured ELF provider missing or exceeds bound: ' + path)
        require(len(records) < 256, 'Dependency closure exceeds provider bound')
        record = inspect(blobs[path], path)
        require(not record['runpaths'], 'Unsupported provider RPATH/RUNPATH: ' + path)
        records[path] = record
        for name in record['needed']:
            candidates = [prefix + '/' + name for prefix in search_roots]
            provider = next((p for p in candidates if p in blobs), None)
            require(provider is not None, 'No captured provider for ' + name + ' required by ' + path)
            if name in by_needed:
                require(by_needed[name] == provider, 'Inconsistent dependency selection: ' + name)
            by_needed[name] = provider
            queue.append(provider)
    indexes, global_symbols = {}, {}
    for path, record in records.items():
        indexes[path] = {}
        for symbol in record['exports']:
            indexes[path].setdefault(symbol['name'], []).append(symbol)
            global_symbols.setdefault(symbol['name'], []).append(symbol)
    checked, optional = 0, 0
    for path, record in records.items():
        for node in record['requiredVersionNodes']:
            require(node['library'] in by_needed and node['version'] in
                    records[by_needed[node['library']]]['definedVersionNames'],
                    'Required provider version absent: ' + path + ':' + node['library'] + '@' + node['version'])
        for item in record['imports']:
            if item['weak']:
                optional += 1
                continue
            if item['library']:
                require(item['library'] in by_needed, 'Versioned provider absent: ' + item['library'])
                candidates = indexes[by_needed[item['library']]].get(item['name'], [])
            else:
                candidates = global_symbols.get(item['name'], [])
            matches = [s for s in candidates if
                       (s['version'] == item['version'] if item['version'] else not s['nonDefault'])]
            require(any(s['type'] == item['type'] or
                        (item['type'] == 'STT_NOTYPE' and s['type'] != 'STT_TLS') or
                        (item['type'] == 'STT_FUNC' and s['type'] == 'STT_LOOS') for s in matches),
                    'Strong import unavailable: ' + path + ':' + item['name'] + '@' + str(item['version']))
            checked += 1
    return dict(providerSha256={p: r['sha256'] for p, r in records.items()},
                selectedDependencies=by_needed, strongImportsChecked=checked,
                optionalWeakImportsNotRequired=optional, staticDependencyPreflightPassed=True,
                hostLibrariesConsulted=False, librariesLoaded=False,
                dynamicDlopenOrPreloadCompatibilityVerified=False, guestRuntimeCompatibilityVerified=False)
