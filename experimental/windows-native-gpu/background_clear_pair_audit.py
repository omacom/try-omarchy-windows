"""Independent scoped audit; original whole-run rejections remain unchanged."""
from pathlib import Path
import hashlib,json,re,sys,subprocess,shutil
assert len(sys.argv)==4, 'Usage: background_clear_pair_audit.py LAB_DIRECTORY REPOSITORY_DIRECTORY NEW_OUTPUT_JSON'
lab=Path(sys.argv[1]).resolve();repo=Path(sys.argv[2]).resolve();output=Path(sys.argv[3])
sys.path.insert(0,str(repo/'experimental/windows-native-gpu'))
from test_owned_runtime_qemu import async_submissions_complete,clear_workload_complete
from broker_runtime_evidence import broker_aware_initialization_complete as initialization_complete
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
build=lab/'background-gpu-clear-v2'
receipt=json.loads((build/'corrected-build-receipt-v1.json').read_bytes())
assert receipt['probeGeometry']==[130,73] and receipt['profile']=='4k'
assert sha(build/'build-receipt.json')==receipt['originalBuildReceiptSha256']
assert all(sha(build/name)==digest for name,digest in receipt['sources'].items())
native=json.loads((lab/'native-v111-receipt.json').read_bytes())
assert all(sha(lab/'native-v111-source-frozen'/name)==digest for name,digest in native['sources'].items())
old_harness=subprocess.check_output([shutil.which('git') or 'C:/Program Files/Git/cmd/git.exe','show','fdad17f7b1e24358d5d9adb2c41336b80a9b4406:experimental/windows-native-gpu/test_owned_runtime_qemu.py'],cwd=repo)
results=[];manifests=[]
for number,label,loads in ((3,'4k',10000),(4,'4k-sampled',2)):
    folder=lab/f'background-gpu-clear-run-{number}'
    report=json.loads((folder/'driver.json').read_bytes());source=json.loads((folder/'source.json').read_bytes())
    guest=(folder/'driver.log').read_text(encoding='utf-8')
    records=[json.loads(line) for line in (folder/'driver.errors.log').read_text().splitlines() if line.startswith('{')]
    c=report['disconnectCleanup'];q=report['ownedQemuControl']
    assert report['diagnosticAccepted'] is False and source['generalVerifierExit']==1
    assert report['hostBridgeExit']==q['qemuExit']==0 and q['ownedQemuExited'] is True and q['qemuForcedStop'] is False
    assert q['fenceControlFailed'] is False and c['driverCleanupVerified'] is True and c['cleanupFailures']==0
    assert all(value==0 for key,value in c.items() if key.startswith(('failed','live','pending','forced')) or key.endswith(('Timeouts','ReleasedAfterVmExit')))
    assert all(value==0 for key,value in q.items() if key.startswith('live') or key.endswith('ReleasedAfterVmExit'))
    assert report['driverBridgeSha256']==source['nativeSha256']==native['bridgeSha256']=='fba526a5ec94f0774136f97cafd546a1ed2f4e099f54399cdc4b2a3c45e00916'
    assert report['qemuSha256']=='5404287396eb03a181941f66f09afaee2bc5859a9b87830d64ea1e9eaa91a87c'
    assert sha(build/f'private-{label}.cpio.gz')==report['initramfsSha256']==source['initramfsSha256']==receipt['images'][label]
    assert source['harnessSha256']==hashlib.sha256(old_harness).hexdigest()
    assert source['harnessUnchanged'] is source['artifactsUnchanged'] is True
    assert source['readOnlyInstalledDriverBefore']==source['readOnlyInstalledDriverAfter']
    assert source['readOnlyInstalledDriverAfter']['installedRuntimeMatchesSnapshot'] is True
    forbidden={'--driver-present-shared','--driver-consume-shared-process','--driver-sdl-relay',
        '--driver-window-input','--driver-input-self-test','--driver-rootfs-readonly','--driver-coordinator'}
    assert not forbidden.intersection(source['command'])
    assert source['sourceDiskAttached'] is False
    assert c['completedSharedTextureConsumes']==c['completedCompositorFrames']==c['openedTextureConsumers']==c['startedNativeGpuWorkers']==0
    assert initialization_complete(guest,report['hostBridgeExit'],q,c)
    queued=[dict(bytes=int(b),privateBytes=int(p),target=int(t),observedAtReturn=int(o)) for b,p,t,o in
        re.findall(r'nativeCommandQueued=true bytes=(\d+) privateBytes=(\d+) target=(\d+) observedAtReturn=(\d+)',guest)]
    accepted=[r for r in records if r.get('nativeAsyncSubmissionAccepted') is True]
    retired=[r for r in records if r.get('nativeAsyncSubmissionRetired') is True]
    assert async_submissions_complete(queued,accepted,retired,c) and len(queued)==7
    assert clear_workload_complete(guest,True)
    probes=[int(v) for v in re.findall(r'LINUX_BRIDGE (?:pagingFenceRetired|allocationCpuMapped|[A-Za-z]+FenceMapped)=true direct=true loads=(\d+)',guest)]
    assert len(probes)==437 and set(probes)=={loads}
    paging=[(int(count),int(target),int(observed),op) for count,target,observed,op in
        re.findall(r'pagingFenceRetired=true direct=true loads=(\d+) target=(\d+) observed=(\d+) operation=(\S+)',guest)]
    assert len(paging)==264 and all(count==loads and 0<=target<=observed<(1<<64)-1 for count,target,observed,op in paging)
    for op,key in (('gpuva','completedVendorGpuVaMaps'),('residency','completedVendorResidencyRequests'),('gpu-state','completedGpuStateMaps')):
        assert sum(row[3]==op for row in paging)==c[key]
    assert c['reportedGpuUsageLimitBytes']==268435456 and c['peakReportedGpuUsageBytes']<=c['reportedGpuUsageLimitBytes']
    m=json.loads((build/f'private-{label}.cpio.manifest.json').read_bytes());manifests.append(m)
    assert m['guestGraphicsBrokerIncluded'] is m['kernelApertureIncluded'] is True and m['guestRenderNodeIncluded'] is False
    results.append(dict(run=number,profile=label,scopedBrokerGpuClearAccepted=True,originalWholeRunAccepted=False,
        surface=[130,73],rounds=2,pixelsPerRound=9490,wholeReadbackPixelsVerified=True,
        originalExpectedHashes=['c53e73aa3030bd71','8bce2f687f7dcf61'],nativeSubmissions=len(queued),
        allNativeSubmissionsIndependentlyRetired=True,pagingRetirementChecks=len(paging),zeroTargetAlreadyRetiredChecks=sum(row[1]==0 for row in paging),
        probeRecords=len(probes),diagnosticReadsPerRecord=loads,diagnosticReadTotal=sum(probes),
        peakReportedGpuUsageBytes=c['peakReportedGpuUsageBytes'],deviceHealthQueries=c['completedDeviceExecutionQueries'],
        hostExit=0,naturalVmExit=True,allTrackedNativeObjectsAndMappingsRetiredBeforeVmExit=True,
        presentationRequested=False,inputRequested=False,sourceDiskAttached=False,
        performanceMeasured=False,dailyUseAccepted=False,
        privateFiles={name:sha(folder/name) for name in ('driver.json','driver.log','driver.errors.log','source.json','source-before.json')}))
a,b=({r['path']:r for r in m['files']} for m in manifests)
assert set(a)==set(b) and [name for name in a if a[name]!=b[name]]==['/linux-ioctl-bridge.so']
value=dict(schema=1,scope='Paired display-free GPU clear/readback; not desktop or performance acceptance',
    sourceCommit='fdad17f7b1e24358d5d9adb2c41336b80a9b4406',runs=results,
    onlyPayloadDifference='/linux-ioctl-bridge.so',surfaceIs4k=False,capacityProfile='4k, 1024 MiB guest RAM',
    earlierAttempts=[dict(run=1,reason='4K executable refused missing profile permissions before VM start',workloadAccepted=False),
        dict(run=2,reason='512 MiB guest initramfs unpack write error, no GPU work',workloadAccepted=False)],
    initialAuditorRejectedValidZeroTargets=True,originalBuildMetadataRetained=True,correctedBuildReceiptSha256=sha(build/'corrected-build-receipt-v1.json'),
    brokerAwareAuditorSha256=sha(repo/'experimental/windows-native-gpu/broker_runtime_evidence.py'),
    originalHarnessSha256=hashlib.sha256(old_harness).hexdigest(),auditorSha256=sha(Path(__file__)))
with output.open('x',encoding='utf-8',newline='\n') as f:json.dump(value,f,indent=2);f.write('\n')
print('PASS: paired genuine NVIDIA clear/readback, broker isolation, both paging-read counts and live cleanup; original general rejections preserved; no desktop or performance acceptance')
