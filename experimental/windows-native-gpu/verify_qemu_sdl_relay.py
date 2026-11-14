"""Independent complete-pixel checks for QEMU's actual SDL pre-swap backbuffer."""
import hashlib
import json
import re
from pathlib import PureWindowsPath
from verify_hyprland_lifecycle_capture import require
from verify_coordinated_gpu_clients import pixel_digest
from verify_sdl_interop import pattern_pixels

PRODUCER_PINS={
    'sourceSha256':'5432a659fb01ffd07ae1369c6e44c27cb499c5d3158fb7047ee460a21f4fa9ec',
    'buildScriptSha256':'a12ca3a0251e2d2dd726668df5d2f1a9090b305f2c3e614a78d94f3af80b895a',
    'binarySha256':'895574da67fb507443b54d13b9bcf746372f5de4d2a1192d8ab99361100b7dd7',
}
RUNNERS={
    '48a8c738f448db7471a4b454df92d9242e689d474efe30610aa188ad2f65ea02',
    'db4b6c88adc34a42ae3b46e1a8898b823a957fa5ac0ac16da1a37abb5ef29f24',
}
# Distinct tested binaries retain their original format and protocol scope.
QEMU_ARTIFACTS={
    'd60af1dfb2211873797de0d2ad872180de247f87d53b4be539777aa5837afbad':dict(
        revision='4503762ef4f93c81cb88ccf69aaeef019119389e',
        archiveSha256='aefb91c98aeb84074fb4f5bc4bf08115fa5aa7a4f430595e3c44c45dda9f8f23',
        graphicsRelaySources={
            'wddm-sdl-relay.c':'3495dd7cbe3ee0f737bd1f327b1b3b1a2504661e343c198a48dca6ef88352531',
            'wddm-sdl-command.h':'0cb5e9982f2a86597324c216c41180867e484124a4589b62286f5bd9575952ac',
        },
        graphicsRelayPatchSha256='421a80bd25e713f0a7c965663e26a85ee2a9525ff2e2bd5059353f241c5e2a86'),
    '5427390c60d90071000b0be5402fcb1f7eaac187e276c21c9544aa7d7ee065d3':dict(
        revision='39f4b02c901c0e5597358ca55f332a34b35fce6e',
        archiveSha256='86f8a801a184182dc1cde56c0e7241f1958bbd3195f7823707ea16a23c2226c1',
        graphicsRelaySources={
            'wddm-sdl-relay.c':'7076f817e318c5a1226c16d3101aa31575061dac65634215058ae63160f6b40c',
            'wddm-sdl-command.h':'0cb5e9982f2a86597324c216c41180867e484124a4589b62286f5bd9575952ac',
        },
        graphicsRelayPatchSha256='421a80bd25e713f0a7c965663e26a85ee2a9525ff2e2bd5059353f241c5e2a86',
        protocol=2),
}
OFFICIAL_INPUT={
    'hw/input/virtio-input-hid.c':'31b9150dc410ad5e2ce48a58f3d0e923dec4e012e62c7be05749786445f1aa2f',
    'include/ui/sdl2.h':'a2c75d65b71bba559706520583aa125ccc0777229ec82e760146d0b1669286d4',
    'ui/input.c':'1b450d44f6825b0d982ba616ff53da8c32481020689aee7659167140bb13dee1',
    'ui/sdl2.c':'1214c6ec9b028fb721bead9f93888807da8c260603b5149a7662a90bacf31ea6',
}
COMPILED_UI={
    'include/ui/console.h':'46c626bc50835e78b8483ccafab276c9fae8f0b89be404f9d220f80f98b95861',
    'ui/console.c':'d500a75264563bb36fec26fd8729447f14957a62cdef9f1ada0106b86e19d169',
    'ui/sdl2-gl.c':'7f2db8743d0b114576c34657234f3e01d3efefd34d03c6f400ce2dd208b50e18',
}


def window_pixels(pixels,width,height,frame):
    require(type(width) is int and type(height) is int and 0<width<=3840 and 0<height<=2160,
            'SDL output is outside the bounded 4K extent')
    require(type(pixels) is bytes and len(pixels)==width*height*4,'Incomplete actual SDL backbuffer')
    colors=((32,64,128,255),(192,32,64,255),(64,192,32,255),(128,64,192,255))
    quadrants=[colors[(i+frame-1)%4] for i in range(4)]
    def average(*values):
        return tuple(sum(value[channel] for value in values)//len(values) for channel in range(4))
    def row(left,right):
        # A smaller odd-sized GL_LINEAR destination has one center seam sample.
        return bytes(left)*(width//2)+(bytes(average(left,right)) if width%2 else b'')+bytes(right)*(width//2)
    bottom=row(quadrants[2],quadrants[3])
    top=row(quadrants[0],quadrants[1])
    middle=row(average(quadrants[0],quadrants[2]),average(quadrants[1],quadrants[3]))
    # glReadPixels returns the lower-left row first; SDL flips the D3D top row.
    expected=bottom*(height//2)+(middle if height%2 else b'')+top*(height//2)
    require(pixels==expected or all(abs(a-b)<=1 for a,b in zip(pixels,expected)),
            'Complete SDL pixels differ from independent flipped/scaled GPU quadrants')
    checksum,digest=pixel_digest(pixels)
    return dict(width=width,height=height,bytes=len(pixels),sha256=digest,fnv1a=f'{checksum:016x}')


def source_pixels(pixels,frame):
    checksum,digest=pattern_pixels(pixels,3840,2160,frame)
    return dict(width=3840,height=2160,bytes=len(pixels),sha256=digest,fnv1a=f'{checksum:016x}')


def validate_qemu_sdl_relay(source,result,frames,native_stdout):
    count=source['frames'];session=source['session']
    require(type(count) is int and 2<=count<=300 and re.fullmatch(r'[0-9a-f]{32}',session) and
            source['generation']==1 and source['sourceImageUsed'] is False,'Wrong owned control/session')
    require(source.get('binariesUnchanged') is True and source.get('sourcesUnchanged') is True and
            source.get('artifactUnchanged') is True,'Source/artifact changed during the control')
    runner=source['runnerSha256'];prefix='experimental/windows-native-gpu/sdl-relay-producer/'
    require(runner in RUNNERS and source['frozenSources']=={
        prefix+'accept_qemu.py':runner,
        prefix+'d3d12_named_producer.cpp':PRODUCER_PINS['sourceSha256'],
        prefix+'build.ps1':PRODUCER_PINS['buildScriptSha256']},'Different controller/build source')
    argv=source['qemuCommand'];producer_argv=source['producerCommand']
    require(len(argv)==23 and PureWindowsPath(argv[0]).is_absolute() and
            PureWindowsPath(argv[0]).name=='qemu-system-x86_64.exe' and argv[1]=='-L' and
            PureWindowsPath(argv[2]).is_absolute() and
            argv[3:6]==['-name','Try-Omarchy-SDL-Relay-'+session,'-machine'] and
            argv[6] in ('q35,accel=tcg','q35,accel=whpx,kernel-irqchip=off') and
            argv[7:14]==['-m','256','-S','-nodefaults','-vga','none','-device'] and
            argv[15:22]==['-device','virtio-keyboard-pci','-device','virtio-tablet-pci',
                         '-display','sdl,gl=on,show-cursor=off,window-close=off','-qmp'] and
            re.fullmatch(r'tcp:127\.0\.0\.1:[1-9][0-9]{0,4}',argv[22]) and
            0<int(argv[22].rsplit(':',1)[1])<65536,'Wrong disk-free QEMU/frontend/owned endpoint')
    artifact=source['artifactReceipt'];qemu_hash=source['qemuSha256']
    require(qemu_hash in QEMU_ARTIFACTS,'This compiled QEMU snapshot has not been accepted')
    pin=QEMU_ARTIFACTS[qemu_hash]
    require(artifact['revision']==pin['revision'] and artifact['archiveSha256']==pin['archiveSha256'] and
            artifact['files']['bin/qemu-system-x86_64.exe']==qemu_hash and
            artifact['files']['bin/SDL2.dll']=='2e44d5bae9a97859eccde14ad4467df14bae7468594bba7fe8eb417f517ce31b',
            'Different QEMU/SDL artifact')
    require(all(source['producerReceipt'].get(key)==digest for key,digest in PRODUCER_PINS.items()) and
            source['producerReceipt']['compiler']=='MSVC /std:c++17 /W4 /WX /O2','Different D3D12 producer')
    provenance=source['qemuProvenance']
    require(provenance['qemuBaseCommit']=='2ce303cfbbc8b0e4a7a3c66e27a094a980426d73' and
            provenance['inputFiles']==OFFICIAL_INPUT and
            len(provenance['officialRuntimePatches'])==24 and
            provenance['graphicsRelaySources']==pin['graphicsRelaySources'] and
            provenance['graphicsRelayPatchSha256']==pin['graphicsRelayPatchSha256'] and
            provenance.get('graphicsRelayProtocolVersion',1)==pin.get('protocol',1),'Mixed graphics/input source')
    require(set(source['compiledSources'])==set(COMPILED_UI)|
            {'hw/display/'+name for name in pin['graphicsRelaySources']},'Missing compiled source/interface')
    for path,capture in source['compiledSources'].items():
        require(hashlib.sha256(capture['utf8'].encode()).hexdigest()==capture['sha256'],
                'Corrupt compiled source snapshot')
        if path.startswith('hw/display/'):
            require(capture['sha256']==provenance['graphicsRelaySources'][path.split('/')[-1]],
                    'Compiled relay source differs')
        else:
            require(capture['sha256']==COMPILED_UI[path],'Different compiled graphics/UI boundary')
    require(result.get('passed') is True and result.get('naturalQemuExit') is True and
            result.get('naturalProducerExit') is True and
            result.get('qemuExitCode')==result.get('producerExitCode')==0 and 'error' not in result,
            'Incomplete/failed host control')
    for row in (source,result):
        require(all(row.get(key) is False for key in ('physicalInputVerified','physicalScanoutVerified',
                    'guestGpuScanoutVerified','nearNativePerformanceVerified')),'Unsupported product claim')
    native=[json.loads(line) for line in native_stdout.splitlines() if line.strip()]
    require(native==result['producerRecords'] and len(native)==count*2+3,'Missing/mixed producer stream')
    ready=native[0]
    require(ready==source['producerReady'] and ready.get('namedProducerReady') is True and
            ready['session']==session and ready['generation']==1 and ready['vendorId']==0x10de and
            'NVIDIA GeForce RTX 5090 Laptop GPU' in ready['adapter'] and
            (ready['width'],ready['height'])==(3840,2160) and
            ready['independentFences'] is True and ready['nonInheritableExports'] is True and
            re.fullmatch(r'[0-9a-f]{16}',ready['adapterLuid']) and
            ready['format']==source['requestedFormat'] and ready['format'] in ('RGBA8','BGRA8'),
            'Wrong hardware, resource or named-object topology')
    require(PureWindowsPath(producer_argv[0]).is_absolute() and
            PureWindowsPath(producer_argv[0]).name=='d3d12-named-producer.exe' and
            producer_argv[1:]==[session,'1']+(['--bgra'] if ready['format']=='BGRA8' else []),
            'Different producer/session/format invocation')
    prefix='wddm-sdl-relay,id=relay,session='+session+',generation=1,adapter-luid='+ready['adapterLuid']+',audit-directory='
    require(argv[14].startswith(prefix),'Wrong named resource/adapter device')
    audit_path=argv[14][len(prefix):]
    protocol=provenance.get('graphicsRelayProtocolVersion',1)
    if protocol>=2:
        suffix=',source-format='+ready['format'].lower()
        require(audit_path.endswith(suffix),'Wrong GPU format selection')
        audit_path=audit_path[:-len(suffix)]
    require(PureWindowsPath(audit_path.replace(',,',',')).is_absolute() and
            PureWindowsPath(audit_path.replace(',,',',')).name=='gpu-buffers','Unowned GPU audit destination')
    require(native[1]=={'namedProducerExportsClosed':True,'handles':3},'Original NT exports remain open')
    require(native[-1]==dict(namedProducerComplete=True,frames=count,exportsClosed=3,
                            gpuObjectsReleased=True,guestFrameVerified=False,nearNativePerformanceVerified=False),
            'Native GPU references not reclaimed')
    usage=[ready['gpuUsageBytes']]
    for number in range(1,count+1):
        queued,retired=native[number*2:number*2+2]
        require(queued==dict(namedProducerFrameQueued=True,frame=number,producerTarget=number,cpuTextureUpload=False),
                'Wrong/stale producer frame or CPU upload')
        require(retired.get('namedProducerConsumerRetired') is True and
                all(type(retired.get(key)) is int and retired[key]==number
                    for key in ('frame','producerFence','consumerFence')),'Independent GPU fences did not retire')
        usage.append(retired['gpuUsageBytes'])
    require(all(type(value) is int and 0<value<=1073741824 for value in usage),'Producer usage guard exceeded')
    states=result['states']
    require(len(states)==count+2,'Missing/extra relay state')
    for index,row in enumerate(states):
        attached=index!=len(states)-1
        require(row['attached'] is attached and row['poisoned'] is False and row['everAttached'] is True and
                row['rootAdapterVerified'] is True and row['generation']==1 and
                row['importContextLive'] is attached and row['localScanoutLive'] is attached and
                row['liveImportedObjects']==(4 if attached else 0),'Incorrect shared reference/context lifecycle')
        require(row['vendor']=='NVIDIA Corporation' and 'NVIDIA GeForce RTX 5090 Laptop GPU' in row['renderer'] and
                row['version'].startswith('4.6.0 NVIDIA ') and row['adapterLuid']==ready['adapterLuid'] and
                (row['sourceWidth'],row['sourceHeight'])==(3840,2160) and
                row['independentFences'] is True and row['cpuTextureUpload'] is False,'Wrong QEMU root GPU/source')
        require(all(row[key] is False for key in ('physicalInputVerified','physicalScanoutVerified',
                    'guestGpuScanoutVerified','nearNativePerformanceVerified')),'Unsupported QEMU display claim')
        expected_frame=min(index,count)
        if protocol>=2:
            bgra=ready['format']=='BGRA8'
            require(row['sourceFormat']==ready['format'].lower() and
                    row['channelProgramLive'] is (bgra and attached) and
                    row['convertedFrames']==(expected_frame if bgra else 0),'Missing/incorrect GPU format conversion')
        require(row['lastQueuedFrame']==expected_frame and
                row['auditFrames']==(0 if index==0 else 2 if index>=count else 1),
                'Missing/stale frame or ordinary pixel readback')
    requests=[row['sent'] for row in result['qmp'] if 'sent' in row]
    received=[row['received'] for row in result['qmp'] if 'received' in row]
    require(sum('QMP' in row for row in received)==1,'Missing/duplicate owned QMP greeting')
    replies={row['id']:row for row in received if 'id' in row}
    require(len(replies)==len(requests) and len([row for row in received if 'id' in row])==len(replies) and
            [row['id'] for row in requests]==list(range(1,len(requests)+1)),'Missing/duplicate QMP acknowledgement')
    position=0
    def call(name,arguments=None,error=False):
        nonlocal position
        require(position<len(requests),'QMP stream truncated')
        request=requests[position];position+=1
        require(request['execute']==name and request.get('arguments')==arguments,'Unexpected QMP command/owner')
        reply=replies[request['id']]
        require(('error' in reply) is error,'Wrong QMP result')
        return reply['error' if error else 'return']
    def command(text,error=False):
        return call('qom-set',dict(path='/machine/peripheral/relay',property='relay-command',value=text),error)
    def state(expected):
        raw=call('qom-get',dict(path='/machine/peripheral/relay',property='relay-state'))
        require(json.loads(raw)==expected,'Published state differs from actual QMP reply')
    call('qmp_capabilities')
    require(call('query-name')['name']=='Try-Omarchy-SDL-Relay-'+session,'QMP owner identity mismatch')
    require(call('query-status')['running'] is False,'VM was not the paused disk-free control')
    require(call('query-display-options')==result['displayOptions']==
            {'gl':'on','window-close':False,'type':'sdl','show-cursor':False},'Wrong SDL frontend options')
    require(any(row['absolute'] and 'tablet' in row['name'].lower() for row in call('query-mice')),
            'Official tablet not discovered')
    rejected=[]
    for text in ('present:1:1','attach:2','attach:01'):
        rejected.append(dict(command=text,error=command(text,True)))
    command('attach:1');state(states[0])
    for text in ('attach:1','present:1:2'):
        rejected.append(dict(command=text,error=command(text,True)))
    for number in range(1,count+1):
        command(('audit' if number in (1,count) else 'present')+':1:'+str(number));state(states[number])
    command('detach:1');state(states[-1])
    rejected.append(dict(command='attach:1',error=command('attach:1',True)))
    call('quit')
    require(position==len(requests) and rejected==result['negativeControls'] and
            all(row['error'].get('desc') for row in rejected),'Unaccounted/missing generation/order controls')
    require(set(frames)=={f'relay-{kind}-{number:016d}.rgba' for kind in ('source','window') for number in (1,count)},
            'Missing/extra full GPU captures')
    captures=[]
    for number in (1,count):
        row=states[number]
        source_proof=source_pixels(frames[f'relay-source-{number:016d}.rgba'],number)
        window_proof=window_pixels(frames[f'relay-window-{number:016d}.rgba'],row['windowWidth'],row['windowHeight'],number)
        require(source_proof['fnv1a']==row['sourceFnv1a'] and window_proof['fnv1a']==row['windowFnv1a'],
                'Complete native GPU pixel digest differs')
        captures.append(dict(frame=number,source=source_proof,window=window_proof))
    return dict(crossProcessGpuImportVerified=True,qemuNvidiaRootVerified=True,
                independentProducerConsumerFencesVerified=True,originalNtExportHandlesClosed=3,
                actualGpuHandoffs=count,completePixelAudits=2,ordinaryGpuOnlyHandoffs=count-2,
                captures=captures,producerPeakReportedGpuBytes=max(usage),producerUsageGuard=1073741824,
                qemuReportedGpuUsageVerified=False,combinedReportedGpuUsageVerified=False,
                explicitGpuDetachVerified=True,naturalShutdownVerified=True,
                guestGpuScanoutVerified=False,physicalInputVerified=False,physicalScanoutVerified=False,
                allFramesDisplayedVerified=False,nearNativePerformanceVerified=False)
