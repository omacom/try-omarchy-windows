"""Verify the bounded host SDL/D3D12 control; never infer QEMU/guest acceptance."""
from pathlib import PureWindowsPath
import re
from verify_hyprland_lifecycle_capture import require
from verify_coordinated_gpu_clients import pixel_digest

BUILD_PINS = {
    'sourceSha256': '2051b2be924875c5861de4149fcb4daef95de455bf670be4111400e906db2a90',
    'bindingHeaderSha256': '945891fce154d2aefb0f2bbf3d39cd836442f107808a6398be0a5121d419bf81',
    'buildScriptSha256': 'ead288baf734978c942ee855bce24b34fa8cb722c602076baf2c525af81de70f',
    'binarySha256': 'eece09b29ce1357883c853083c5f78c75a52f5e4d6b950865bc7f8ffadf0a7e9',
}
SDL_LIBRARY = '2e44d5bae9a97859eccde14ad4467df14bae7468594bba7fe8eb417f517ce31b'


def pattern_pixels(pixels, width, height, frame):
    require(type(pixels) is bytes and len(pixels)==width*height*4,'Incomplete interop GPU pixels')
    colors=(bytes((32,64,128,255)),bytes((192,32,64,255)),bytes((64,192,32,255)),bytes((128,64,192,255)))
    q=[colors[(index+frame-1)%4] for index in range(4)]
    top=q[0]*(width//2)+q[1]*(width-width//2)
    bottom=q[2]*(width//2)+q[3]*(width-width//2)
    expected=top*(height//2)+bottom*(height-height//2)
    require(pixels==expected or all(abs(a-b)<=1 for a,b in zip(pixels,expected)),
            'Interop GPU bytes differ from the independent D3D12 quadrant pattern')
    return pixel_digest(pixels)


def validate_sdl_interop(source, records, frames):
    require(source.get('mode')=='full' and source.get('exitCode')==0 and source.get('timedOut') is False and
            source.get('sourcesUnchanged') is True,'Host interop did not complete against frozen sources')
    receipt=source['buildReceipt']
    require(all(receipt.get(name)==digest for name,digest in BUILD_PINS.items()) and
            receipt.get('compiler')=='MSVC /std:c++17 /W4 /WX /O2','Mixed interop source/header/build/binary receipt')
    require(source.get('sdlLibrarySha256')==SDL_LIBRARY and
            source.get('qemuArtifactRevision')=='a42cab73866636ed8a99af69446a5bf66d97adbe','Different SDL dependency')
    command=source['command']
    require(len(command)==3 and PureWindowsPath(command[0]).name=='sdl-d3d12-interop.exe' and
            PureWindowsPath(command[1]).name.lower()=='sdl2.dll' and
            all(PureWindowsPath(part).is_absolute() for part in command),'Missing bounded owned interop invocation')
    keys=('sdlGlContext','sdlGlExternalCapabilities','sdlD3D12ObjectsOpened',
          'sdlD3D12InteropFrame','sdlD3D12InteropFrame','sdlD3D12InteropComplete')
    require(len(records)==len(keys) and all(row.get(key) is True for row,key in zip(records,keys)),
            'Missing/failed/duplicate interop lifecycle')
    context,caps,opened,first,last,closed=records
    require(all(row.get('vendor')=='NVIDIA Corporation' and
                'NVIDIA GeForce RTX 5090 Laptop GPU' in row.get('renderer','') and
                row.get('version','').startswith('4.6.0 NVIDIA ') for row in (context,caps)) and
            all(context.get(key)==caps.get(key) for key in ('vendor','renderer','version')),
            'Different/non-NVIDIA OpenGL context')
    require(caps.get('sameAdapterLuid') is True and caps.get('vendorId')==0x10de and
            caps.get('windowHidden') is True and caps.get('qemuFrontendVerified') is False and
            type(caps.get('gpuUsageBytes')) is int and 0<caps['gpuUsageBytes']<=1073741824,
            'Adapter mismatch or unsupported display/budget claim')
    require((opened.get('sourceWidth'),opened.get('sourceHeight'),opened.get('windowWidth'),opened.get('windowHeight'))==
            (3840,2160,1280,720) and all(opened.get(key) is True for key in
            ('originalNtHandlesClosed','namedObjects','independentFences')),'Wrong named shared-object topology')
    require(set(frames)=={f'interop-{kind}-{frame}.rgba' for kind in ('source','window') for frame in (1,2)},
            'Missing/extra interop pixel captures')
    proof=[]
    for number,row in enumerate((first,last),1):
        require(type(row.get('frame')) is int and row['frame']==number and
                all(type(row.get(key)) is int and row[key]==number for key in
                    ('producerFence','consumerFence','observedProducerFence','observedConsumerFence')) and
                row.get('independentFences') is True,'Unretired/mixed producer or consumer fence')
        require((row.get('sourceWidth'),row.get('sourceHeight'),row.get('windowWidth'),row.get('windowHeight'))==
                (3840,2160,1280,720) and row.get('cpuUpload') is False and row.get('diagnosticReadbacks')==2 and
                row.get('displayVerified') is False and type(row.get('gpuUsageBytes')) is int and
                0<row['gpuUsageBytes']<=1073741824,'Wrong GPU copy/readback path or usage budget')
        captures=[]
        for kind,width,height,key in (('source',3840,2160,'sourceFnv1a'),('window',1280,720,'windowFnv1a')):
            checksum,digest=pattern_pixels(frames[f'interop-{kind}-{number}.rgba'],width,height,number)
            require(isinstance(row.get(key),str) and re.fullmatch(r'[0-9a-f]{1,16}',row[key]) and
                    int(row[key],16)==checksum,'Complete interop pixel digest differs')
            captures.append(dict(kind=kind,width=width,height=height,rawSha256=digest,fnv1a=f'{checksum:016x}'))
        proof.append(dict(frame=number,producerFence=number,consumerFence=number,captures=captures))
    require(closed.get('frames')==2 and closed.get('sharedHandlesClosed')==3 and
            all(closed.get(key) is True for key in ('sameAdapterLuid','namedObjects','independentFences','gpuOnlyTransfer',
                                                   'glObjectsReleased','d3dObjectsReleased','sdlClosed')) and
            all(closed.get(key) is False for key in ('qemuFrontendVerified','guestFrameVerified','nearNativePerformanceVerified')),
            'Incomplete shared-object cleanup or unsupported acceptance claim')
    require(source.get('qemuFrontendVerified') is False and source.get('guestFrameVerified') is False,
            'Host receipt claims guest/frontend acceptance')
    return dict(hostSdlD3D12InteropVerified=True,sameNvidiaAdapterVerified=True,namedObjectsVerified=True,
                independentGpuFencesVerified=True,frames=proof,originalSharedHandlesClosed=3,
                cpuUpload=False,diagnosticReadbacks=4,windowHidden=True,
                qemuFrontendVerified=False,guestFrameVerified=False,physicalInputVerified=False,
                allFramesDisplayedVerified=False,nearNativePerformanceVerified=False)
