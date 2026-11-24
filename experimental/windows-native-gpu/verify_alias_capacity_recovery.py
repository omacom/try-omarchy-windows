"""Keep transport refusal controls separate from real GPU recovery acceptance."""
from pathlib import Path
import hashlib
from verify_hyprland_lifecycle_capture import require

BASE = '5133d26db61ae839b4cc22057f8606ccb3b47ab953ba241c3d221b4fb949ad7e'
MANIFEST = '608d03bcbefa0ae05240013706305de9d629a30e32fddeb8f82a21f1a1144639'
GUEST = '943994ddf6ef496009519f1f9f9def5157102a9c7dbb8b570c3e2b4d6e9acdfd'


def validate_refusal_controls(before, after):
    require('BROKER_ROUTE rejected=true errno=12 operation=5 reason=Mock native alias refusal:' in before and
            'Resolution refusal child: Broker connection stopped:' in before and
            'Check failed: serve(peer,native,memory,objects)==(recoverable ? 0 : 1)' in before,
            'Missing original capacity-disconnect regression')
    for error, recoverable, malformed in ((12,1,0),(28,1,0),(5,0,0),(110,0,0),(71,0,1)):
        marker = (f'BROKER_RESOLVE_REFUSAL errno={error} recoverable={recoverable} malformed={malformed} '
                  f'sameConnectionRetry={recoverable} liveFdPreserved=true pidfdExitRequired=true '
                  'native=mock kernel=mock gpu=false')
        require(after.count(marker) == 2, 'Missing default/4K real IPC control: ' + marker)
    require(after.count('refusedAliasLeavesNoCache=true capacityRetry=true aliasCached=true producerExitRetained=true') == 2 and
            'BROKER_RPC_CONTRACT clientUid=65534 readonlyFd=true retainedAfterEof=true attack=rejected' in after and
            'BROKER_REPLY_DISCONNECT validRequest=true replyLost=true pidfdExitRequired=true' in after and
            'skipped=root-required' not in after, 'Required ownership or root RPC controls were skipped')
    return dict(realUnixCredentialsAndFdPassingVerified=True, sameConnectionCapacityRetryVerified=True,
                originalDisconnectReproduced=True, liveSenderFdPreserved=True, prematureAliasPublicationRejected=True,
                protocolAndUnexpectedDriverErrorsRemainFatal=True, defaultAnd4kProfilesVerified=True,
                nativeGpuOperationsMocked=True, gpuAdmissionFailureRecoveryVerified=False)


def validate_alias_capacity_desktop(**inputs):
    path = Path(__file__).resolve().parent/'verify_multiapp_desktop.py'
    raw = path.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == BASE, 'Frozen CPU384 auditor changed')
    text = raw.decode('utf-8')
    for old,new in [
        ("MANIFEST = '6bd54199bed5dcef9eebf92f49835103faff23d73c0c5dfec0a1c6062efcc92a'",f"MANIFEST = '{MANIFEST}'"),
        ("GUEST = '79c64bf444fd604b6559d790eb8382b0fc292f8641b2eafd458206d5e5f87d68'",f"GUEST = '{GUEST}'")]:
        require(text.count(old) == 1, 'Candidate auditor derivation boundary changed')
        text = text.replace(old,new,1)
    namespace = {'__name__':'_alias_capacity_desktop','__file__':str(path)}
    exec(compile(text,str(path),'exec'),namespace)
    require(inputs['source']['aliasCapacityRecoveryEnabled'] is True, 'Recovery endpoints were not requested')
    proof = namespace['validate_multiapp_desktop'](**inputs)
    proof.update(aliasCapacityReplyCandidateVerified=True, allQuotasUnchanged=True,
                 gpuAdmissionFailureRecoveryVerified=False, intermittentAliasLimitResolved=False)
    return proof
