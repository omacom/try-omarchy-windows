"""Check metadata IPC reduction separately from unchanged desktop acceptance."""
from pathlib import Path
import hashlib
from verify_hyprland_lifecycle_capture import require

BASE='5133d26db61ae839b4cc22057f8606ccb3b47ab953ba241c3d221b4fb949ad7e'
MANIFEST='393daeba659f53b1048413b438c9af193aad33b3e251a604c684480170c6811b'
GUEST='e9eed0cf1bf71a87530b2687ea037229d7a63a36d8f538b2d162444f6786b928'


def validate_metadata_controls(before,after):
    require(before.count('APERTURE_INFO_REQUESTS broken=0 layouts=7 capacities=30 profiles=24')==1 and
            'APERTURE_INFO_CONTROL_FAILURE' in before and 'BASELINE_CONTROL_EXIT 1' in before,
            'Missing redundant-IPC baseline failure')
    require(after.count('APERTURE_INFO_REQUESTS broken=0 layouts=7 capacities=6 profiles=6')==2,
            'Missing strict default/CPU384 reduced request counts')
    for broken,caps,profiles in ((1,0,0),(2,0,0),(3,1,0),(4,1,1)):
        require(after.count(f'APERTURE_INFO_REQUESTS broken={broken} layouts=2 capacities={caps} profiles={profiles}')==2,
                'Malformed layout/agreement control missing')
    marker=('APERTURE_INFO_COALESCING perCallOnly=true freshCounters=true concurrentCalls=true '
            'closedChannelRejected=true malformedLayoutsAndAgreementsRejected=true uid=65534 gpu=false kernel=mock')
    require(after.count(marker)==2,'Required real-credential IPC controls missing')
    return dict(realAuthenticatedUnixIpcVerified=True,clientUid=65534,validations=6,
                immutableMetadataRequestsBefore=54,immutableMetadataRequestsAfter=12,
                freshLayoutRequestsBefore=7,freshLayoutRequestsAfter=7,
                crossCallCaching=False,concurrentFreshCountersVerified=True,
                malformedAndClosedChannelsRejected=True,nativeGpuCallsMocked=True,
                performanceImprovementEstablished=False)


def _validate_metadata_desktop(expected_manifest,expected_guest,**inputs):
    path=Path(__file__).resolve().parent/'verify_multiapp_desktop.py';raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==BASE,'Frozen CPU384 auditor changed')
    source=inputs['source'];build=source['manifest']['apertureInfoCoalescingBuild']
    require(source['apertureInfoCoalescingEnabled'] is True and source['aliasCapacityRecoveryEnabled'] is True and
            build['freshMutableCounters'] is build['ordinaryBrokerByteIdentical'] is build['allQuotasUnchanged'] is True and
            build['cacheLifetime']=='single info() validation only' and
            build['nativeBackendChanged'] is build['inputChanged'] is build['profilingEnabled'] is False and
            build['originalDesktopWorkloadUnchanged'] is True and build['originalAuditFrames']==[1,1800] and
            source['manifest']['files']['bin/guest-graphics-broker']['sha256']=='c6bced9dc18e7cafb0eb7868ed7d7c6ed1cc09e974d9b8d01c2f504e0dca54b8' and
            source['manifest']['files']['lib/linux-ioctl-bridge.so']['sha256']=='f25f27b9a5a8ce6afe036a2ec45a089c30c4b985c59e09bb3f499d935f4a8e6f',
            'Wrong optimized guest or changed workload, input or quota')
    text=raw.decode('utf-8')
    for old,new in [
        ("MANIFEST = '6bd54199bed5dcef9eebf92f49835103faff23d73c0c5dfec0a1c6062efcc92a'",f"MANIFEST = '{expected_manifest}'"),
        ("GUEST = '79c64bf444fd604b6559d790eb8382b0fc292f8641b2eafd458206d5e5f87d68'",f"GUEST = '{expected_guest}'")]:
        require(text.count(old)==1,'Candidate auditor derivation changed');text=text.replace(old,new,1)
    namespace={'__name__':'_coalesced_metadata_desktop','__file__':str(path)}
    exec(compile(text,str(path),'exec'),namespace)
    proof=namespace['validate_multiapp_desktop'](**inputs)
    proof.update(coalescedMetadataGuestCompatibilityVerified=True,allQuotasUnchanged=True,
                 memoryPressureRecoveryVerified=False,performanceImprovementVerified=False,
                 actualNativeQuotaRefusalRecoveryVerified=False,fullDesktopAccepted=False)
    return proof


def validate_coalesced_metadata_desktop(**inputs):
    return _validate_metadata_desktop(MANIFEST,GUEST,**inputs)


def validate_framed_metadata_desktop(**inputs):
    source=inputs['source'];build=source['manifest']['traceFramingBuild']
    require(source['traceFramingEnabled'] is True and
            build['changedPayloadFiles']==['lib/hyprland-startup-hook.so'] and
            build['inputChanged'] is build['renderingChanged'] is False and
            build['allQuotasUnchanged'] is True,'Changed framing experiment scope')
    proof=_validate_metadata_desktop(
        '2185b3547247dd6ad7224795af04e5c0d0c2656af8fa8c085a840726ead0ef57',
        '564839aae7e73fc114c2f85ed60e0d18c0933928c6263f2b60eb998a03cc8371',**inputs)
    proof.update(atomicGuestReceiptFramingVerified=True,traceRepairedAfterCapture=False)
    return proof
