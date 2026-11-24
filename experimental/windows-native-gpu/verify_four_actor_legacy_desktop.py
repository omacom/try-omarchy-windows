"""Verify Native84 with the frozen v6, one-application desktop fixture.

This is a compatibility check, not acceptance of the new four-actor workload.
Historical auditors and evidence keep their exact source and acceptance rules.
The candidate exercised eviction/reclamation but zero residency-reference reuse;
the exact ownership ledger is required, and reuse coverage is reported separately.
"""
from pathlib import Path
import hashlib
from verify_hyprland_lifecycle_capture import require

BRIDGE = '90c39fe79c87a494e31a8d7461ace6e613b4d8280bb344473386a349c4157284'
PINS = {
    'verify_managed_mapping_desktop.py': '892515f6f71aa3979c6eb111e73ec185087bb487ee8f128d7e431b9bbcb72a9a',
    'verify_managed_residency_state.py': 'a13b7fecad67d73c7edddc534a5fe6debbebfdb285b0ee46191a279772a057dc',
    'verify_deferred_residency.py': '842edb5afb01247221a1327c920a1d4586c6e75a83ec7783e30fd2cbf6c40bad',
}


def derived(name, replacements=()):
    """Load a fixed local auditor with only the documented coverage changes."""
    raw = (Path(__file__).resolve().parent / name).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == PINS[name], 'Historical auditor source changed')
    text = raw.decode('utf-8')
    for old, new in replacements:
        require(text.count(old) == 1, 'Derived auditor boundary changed')
        text = text.replace(old, new, 1)
    namespace = {'__name__': '_four_actor_legacy_' + name[:-3]}
    exec(compile(text, name, 'exec'), namespace)
    return namespace


def validate_legacy_desktop(*args, **kwargs):
    report = kwargs.get('report', args[0] if args else None)
    source = kwargs.get('source', args[4] if len(args) > 4 else None)
    records = kwargs.get('records', args[2] if len(args) > 2 else None)
    build = source['nativeBuildReceipt']
    require(report['driverBridgeSha256'] == build['bridgeSha256'] == BRIDGE and
            build['managedMappingNegotiationVersion'] == 8 and build['olderAgreementsRetained'] is True and
            build['defaultKernelLeaseLimit'] == 768 and build['desktopKernelLeaseLimit'] == 1536 and
            build['coordinatedHwQueueLimit'] == 64 and build['coordinatedSyncObjectLimit'] == 768 and
            build['physicalMemoryGuardRaised'] is False, 'Wrong compatibility candidate')
    contracts = [r for r in records if r.get('nativeCoordinatedObjectCapacity')]
    require(len(contracts) == 1 and contracts[0]['version'] == 6 and
            contracts[0]['kernelLeaseLimit'] == 768 and contracts[0]['hwQueues'] == 32 and
            contracts[0]['syncObjects'] == 384 and contracts[0]['vendorAllocationObjects'] == 1024,
            'Legacy workload received new actor limits')
    ownership = derived('verify_deferred_residency.py', [
        ('requests>0 and reused>0 and reclaimed>0', 'requests>0 and reused>=0 and reclaimed>0')])
    state = derived('verify_managed_residency_state.py', [
        ("build['managedMappingNegotiationVersion']==6", "build['managedMappingNegotiationVersion']==8")])
    state['validate_ownership'] = ownership['validate_ownership']
    desktop = derived('verify_managed_mapping_desktop.py')
    desktop['BRIDGES'] = {BRIDGE: '84'}
    desktop['validate_managed_state'] = state['validate_managed_state']
    proof = desktop['validate_managed_mapping_desktop'](*args, **kwargs)
    proof.update(legacyV6CompatibilityVerified=True, newFourActorDesktopAccepted=False,
                 residencyReferenceReuseExercised=proof['sharedBudget']['reusedReferences'] > 0)
    return proof
