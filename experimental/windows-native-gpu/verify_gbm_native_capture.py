"""Audit the owned render node, GBM buffers and sampled GPU pixels; no desktop gate."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from triangle_evidence import WIDTH, HEIGHT, TOLERANCE, checksum, triangle_reference
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_gbm_native(report, guest, records):
    result=validate_kernel_aperture(report,guest,records,transport='authenticated-broker',contract_checks=583)
    lines=guest.splitlines()
    def once(line):
        require(lines.count(line)==1,'Missing or duplicated GBM receipt: '+line)
        return lines.index(line)
    require(not any(s in guest for s in ('GBM_NATIVE_FAILURE','Oops:','BUG:','cleanup=false')),'GBM, kernel or native failure')
    require(exact(report,liveNvidiaLinuxUmdLoaded=True,realWslDxgForwarding=False,capturedPrivateFixturesUsed=False),'Wrong live GPU provenance')
    once('BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    node=once('KERNEL_RENDER_START node=/dev/dri/renderD128 defaultDisabled=true nativeRoute=true kms=false gem=false')
    once('KERNEL_RENDER_NODE_QUERY node=/dev/dri/renderD128 result=0 errno=0 version=1 flags=1 signature=003152444d444457 width=4096 height=4096')
    contract=once('GBM_NATIVE_CONTRACT malformedInfo=7 readOnlyCopyout=true kernelPrime=false kernelSyncobj=false gemRejected=true cpuMapRejected=true')
    device=once('GBM_NATIVE_DEVICE node=/dev/dri/renderD128 drmName=/dev/dri/renderD128 backend=drm realDrmFd=true desktop=false')
    storage=[]
    for n,(width,height,fourcc,dxgi,rgba) in enumerate(((1281,719,'34325241',87,bytes((255,0,0,255))),
                                                    (257,131,'34324241',28,bytes((0,255,0,255))))):
        digest=checksum(rgba*(width*height))
        storage.append(once(f'GBM_NATIVE_BUFFER index={n} width={width} height={height} fourcc={fourcc} stride={width*4} modifier=00ffffffffffffff nativeDmaBuf=true sourceBoDestroyed=true imageDestroyed=true textureAlive=true verifiedPixels={width*height} rgba={rgba.hex()} fnv1a={digest}'))
        once(f'LINUX_BRIDGE nativeInteropResource=true threadOwned=true width={width} height={height} format={dxgi}')
        probes=[r for r in records if r.get('nativeSharedResourceImportProbe') is True and r.get('width')==width]
        require(len(probes)==1 and exact(probes[0],verified=True,width=width,height=height,format=dxgi,
            flags=1,dimension=3,sharedNtHandleClosed=True,importedResourceReleased=True),'Native GBM resource description mismatch')
    require(sum(l.startswith('GBM_NATIVE_BUFFER ') for l in lines)==2,'Mixed or duplicate GBM buffers')
    bound=once('GBM_NATIVE_BOUND shaderTarget=0 viewport=130x73 texture=1281x719 concurrentBos=2 kms=false desktop=false')
    sampled=once('GBM_NATIVE_SAMPLED source=0 destination=1 sourceFourcc=34325241 destinationFourcc=34324241 width=130 height=73 referenceFrame=2 topOrigin=true independentPixelAudit=pending')
    rows=re.findall(r'^GBM_NATIVE_SAMPLED_PIXEL_ROW y=(\d+) rgba=([0-9a-f]+)[ \t\r]*$',guest,re.M)
    require(len(rows)==HEIGHT and guest.count('GBM_NATIVE_SAMPLED_PIXEL_ROW ')==HEIGHT,'Missing or duplicated sampled GPU pixels')
    pixels=bytearray()
    for y,(row,rgba) in enumerate(rows):
        require(row==str(y) and len(rgba)==WIDTH*8,'Malformed or reordered sampled GPU pixels')
        pixels.extend(bytes.fromhex(rgba))
    reference,coverage=triangle_reference(2)
    maximum=0
    for index,(observed,expected) in enumerate(zip(pixels,reference)):
        error=abs(observed-expected)
        require(error<=(TOLERANCE if coverage[index//4] and index%4<3 else 0),'Independent sampled GPU pixel mismatch')
        maximum=max(maximum,error)
    sampling=once('GBM_NATIVE_SAMPLING_COMPLETE importedTextureSampled=true distinctDestination=true softwarePixels=false independentPixelAudit=pending')
    complete=once('GBM_NATIVE_COMPLETE allocation=true fdExport=true eglImport=true shaderAudit=pending created=73 released=73 held=0 bytes=0 beforeProcessExit=true desktop=false')
    renderExit=once('KERNEL_RENDER_EXIT unloaded=true nodeRemoved=true')
    buffers=once('KERNEL_NATIVE_BUFFER_EXIT held=0 bytes=0 created=73 released=73 cleanup=true')
    require(node<contract<device<storage[0]<storage[1]<bound<sampled<sampling<complete<renderExit<buffers,'Wrong GBM allocation/render/lifetime ordering')
    require(bound<once('OPENGL_FRAME frame=1 vertices=3 error=0000')<once('OPENGL_FRAME frame=2 vertices=3 error=0000')<sampled,
        'Sampling predates source shader')
    require(sampled<once('GBM_NATIVE_SAMPLED_PIXEL_ROW y=0 rgba='+rows[0][1])<
        once('GBM_NATIVE_SAMPLED_PIXEL_ROW y=72 rgba='+rows[-1][1])<sampling,'Pixels outside sampled draw')
    clean,vm=report['disconnectCleanup'],report['ownedQemuControl']
    require(all(type(v) is int and v==0 for k,v in clean.items() if k.startswith('failed') or k.endswith('Timeouts')),'Native failure or timeout')
    require(exact(clean,sharedResourceProbes=2,importedSharedResources=2,failedSharedResourceProbes=0),'Wrong native metadata imports')
    require(all(vm[k]==0 for k in ('fenceMappingsReleasedAfterVmExit','allocationMappingsReleasedAfterVmExit','allocationSlotMappingsReleasedAfterVmExit')),'Cleanup deferred until VM exit')
    sync=re.findall(r'^KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=(\d+) signaled=(\d+) errors=0 released=(\d+) cleanup=true[ \t\r]*$',guest,re.M)
    require(len(sync)==1,'Missing sync-file cleanup')
    created,signaled,released=map(int,sync[0])
    require(created==signaled==released>0 and exact(clean,acceptedHwQueueSignals=created,completedHwQueueSignals=created,pendingHwQueueSignals=0),'Native GPU signal retirement mismatch')
    result.update(nativeRenderNodeVerified=True,gbmAllocationVerified=True,gbmDmaBufExportVerified=True,
        eglGbmPlatformVerified=True,ownedImportedTextureSamplingVerified=True,dmaBufVerified=True,
        fullDrmDeviceVerified=False,kmsVerified=False,kernelPrimeVerified=False,implicitBufferSynchronizationVerified=False,
        desktopVerified=False,nearNativePerformanceVerified=False,verifiedStoragePixels=1281*719+257*131,
        sampledFrame={'width':WIDTH,'height':HEIGHT,'verifiedPixels':WIDTH*HEIGHT,'maximumChannelError':maximum,
            'sha256':hashlib.sha256(pixels).hexdigest(),'fnv1a':checksum(pixels)},
        nativeBuffersCreated=73,nativeBuffersReleased=73,renderModuleUnloaded=True)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    report=json.loads(args.report.read_text()); guest=args.report.with_suffix('.log').read_text()
    records=[json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    result=validate_gbm_native(report,guest,records)
    with args.output.open('x',encoding='utf-8') as output: json.dump(result,output,indent=2); output.write('\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__': main()
