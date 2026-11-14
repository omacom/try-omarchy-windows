"""Check real Go-generated launcher profiles against the native parser; no VM/GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('go-host', 'native-probe', 'binding-fixture', 'bridge', 'default-bridge', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(exist_ok=False)
    binaries = {name: getattr(args, name).resolve(strict=True) for name in ('go_host', 'native_probe', 'binding_fixture', 'bridge', 'default_bridge')}
    hashes = {name: sha(path) for name, path in binaries.items()}
    options = dict(creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    exported = output / 'profiles.json'
    env = os.environ.copy(); env['OMARCHY_NATIVE_LAUNCHER_PROFILE_OUTPUT'] = str(exported)
    with (output / 'go-export.log').open('x') as log:
        subprocess.run([str(binaries['go_host']), '-test.run=^TestNativeLauncherProfileExport$', '-test.v'], env=env,
            stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30, **options)
    result = subprocess.run([str(binaries['native_probe']), str(exported)], capture_output=True, text=True, check=True, timeout=10, **options)
    proof = json.loads(result.stdout)
    expected = dict(actualGoProfiles=6, accepted=3, rejected=3, unicodeRoundtrip=True,
        originalBootInputPreserved=True, qemuStarted=False, gpu=False, normalGuestBootVerified=False)
    if proof != expected: raise ValueError('Wrong launcher profile proof scope')
    env['OMARCHY_NATIVE_BINDING_FIXTURE'] = str(binaries['binding_fixture'])
    with (output / 'binding-fixture.log').open('x') as log:
        subprocess.run([str(binaries['go_host']), '-test.run=^TestNativeLauncherBindingFixture$', '-test.v'], env=env,
            stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60, **options)
    rows = json.loads(exported.read_bytes())
    source = next(row for row in rows if row['name'] == 'cursor-off')
    checks = []
    for label, binary in (('high-resolution', binaries['bridge']), ('default', binaries['default_bridge'])):
        prefix = [str(binary), '--run-qemu', *(['launcher-owned'] * 5), '--driver-contexts', '--driver-queries']
        cases = [
            ([str(binary), '--stdio', '--driver-launcher-profile', '{}'], 'requires inherited lifetime control'),
            (prefix + ['--driver-launcher-cancel-handle', '1', '--driver-launcher-profile', '{"protocol":1,"protocol":1}'], 'Duplicate launcher profile field'),
            (prefix + ['--driver-launcher-cancel-handle', '1', '--driver-launcher-profile', '{}'], 'Invalid launcher profile schema'),
            (prefix + ['--driver-launcher-cancel-handle', '1', '--driver-launcher-profile', '{}', '--driver-launcher-profile', '{}'], 'Launcher profile must be specified once')]
        for index, (command, reason) in enumerate(cases):
            child = subprocess.run(command, capture_output=True, text=True, timeout=5, **options)
            if child.returncode == 0 or reason not in child.stderr or 'ownedQemuPid' in child.stdout:
                raise ValueError('Wrong profile CLI rejection: ' + child.stdout + child.stderr)
            name = label + '-' + str(index) + '.log'
            with (output / name).open('x') as log: log.write(child.stdout + child.stderr)
            checks.append(dict(profile=label, index=index, rejected=True, qemuStarted=False, logSha256=sha(output / name)))
    # Consume the exact flags prepared by Go, changing only its audit destination.
    # An invalid non-inherited token must be rejected before Runtime/VM creation.
    prepared = list(source['prepared']); prepared[0] = str(binaries['bridge'])
    index = prepared.index('--driver-launcher-profile') + 1
    profile = json.loads(prepared[index]); profile['auditDirectory'] = str(output)
    prepared[index] = json.dumps(profile, ensure_ascii=True, separators=(',', ':'))
    prepared += ['--driver-launcher-cancel-handle', '1']
    child = subprocess.run(prepared, capture_output=True, text=True, timeout=5, **options)
    if child.returncode == 0 or 'Cancellation capability was not inherited' not in child.stderr or 'ownedQemuPid' in child.stdout:
        raise ValueError('Full Go profile did not reach lifetime validation before startup: ' + child.stdout + child.stderr)
    with (output / 'full-profile-invalid-capability.log').open('x') as log: log.write(child.stdout + child.stderr)
    boot_prepared = list(prepared)
    if '--driver-launcher-session' not in boot_prepared:
        boot_prepared += ['--driver-launcher-session']
    boot_prepared += ['--driver-boot-console']
    child = subprocess.run(boot_prepared, capture_output=True, text=True, timeout=5, **options)
    if child.returncode == 0 or 'Cancellation capability was not inherited' not in child.stderr or 'ownedQemuPid' in child.stdout:
        raise ValueError('Boot-console Go profile did not reach lifetime validation before startup: ' + child.stdout + child.stderr)
    with (output / 'boot-profile-invalid-capability.log').open('x') as log: log.write(child.stdout + child.stderr)
    if hashes != {name: sha(path) for name, path in binaries.items()}: raise ValueError('Compiled artifact changed')
    receipt = dict(schema=1, proof=proof, cliChecks=checks, exactGoFlagsReachedLifetimeValidation=True,
        bootConsoleGoFlagsReachedLifetimeValidation=True,
        bootProfileInvalidCapabilityLogSha256=sha(output / 'boot-profile-invalid-capability.log'),
        fullProfileInvalidCapabilityLogSha256=sha(output / 'full-profile-invalid-capability.log'),
        binaryHashes=hashes, exportedProfilesSha256=sha(exported), goExportLogSha256=sha(output / 'go-export.log'),
        bindingFixtureLogSha256=sha(output / 'binding-fixture.log'), earlyBindingCpuModes=5,
        verifierSha256=sha(Path(__file__)), windowsOpened=0, hostInputSent=False, guestDiskAttached=False,
        nativeOriginalArgumentVmTested=False, normalGuestBootVerified=False, dailyUseAccepted=False)
    with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(receipt, stream, indent=2); stream.write('\n')
    print('PASS six actual Go/native profiles, Unicode and exact CLI contract; no VM/GPU/window/input')


if __name__ == '__main__': main()
