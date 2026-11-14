"""Audit the bounded shared-context host control, retaining the original proof."""
from pathlib import PureWindowsPath
from verify_hyprland_lifecycle_capture import require
from verify_sdl_interop import SDL_LIBRARY, pattern_pixels

SHARED_BUILD = dict(
    sourceSha256='163c593538c85df564249a4fdfa24f8de152a2a2376a7f2c65b6a8aea2f47677',
    binarySha256='53abdf370021348c56e2712eb5d658bafae5001294f7a35d80ecc69e9a036053',
    bindingHeaderSha256='945891fce154d2aefb0f2bbf3d39cd836442f107808a6398be0a5121d419bf81',
    buildScriptSha256='ead288baf734978c942ee855bce24b34fa8cb722c602076baf2c525af81de70f',
)


def validate_shared_context(source,records,frames):
    shared=source.get('mode')=='shared-contexts'
    require(source.get('mode') in ('shared-contexts','full') and source.get('exitCode')==0 and
            source.get('timedOut') is False and source.get('sourcesUnchanged') is True,'Failed/partial host control')
    build=source['buildReceipt']
    require(all(build.get(key)==digest for key,digest in SHARED_BUILD.items()) and
            build.get('compiler')=='MSVC /std:c++17 /W4 /WX /O2','Different shared-context build')
    require(source.get('sdlLibrarySha256')==SDL_LIBRARY and
            source.get('qemuArtifactRevision')=='a42cab73866636ed8a99af69446a5bf66d97adbe','Different SDL dependency')
    command=source['command']
    require(len(command)==(4 if shared else 3) and
            all(PureWindowsPath(part).is_absolute() for part in command[:3]) and
            PureWindowsPath(command[0]).name=='sdl-d3d12-interop.exe' and
            PureWindowsPath(command[1]).name.lower()=='sdl2.dll' and
            (not shared or command[3]=='--shared-contexts'),'Wrong owned invocation')
    keys=['sdlGlContext']+(['sdlGlSharedImportContext'] if shared else [])+[
        'sdlGlExternalCapabilities','sdlD3D12ObjectsOpened','sdlD3D12InteropFrame',
        'sdlD3D12InteropFrame','sdlD3D12InteropComplete']
    require(len(records)==len(keys) and all(row.get(key) is True for row,key in zip(records,keys)),
            'Missing/duplicate shared-context lifecycle')
    root=records[0]
    caps,opened,first,last,closed=records[-5:]
    # The shared form contains a child-context record before the common tail.
    if shared:
        child=records[1]
        require(child.get('distinctContexts') is True and child.get('vendor')==root.get('vendor') and
                child.get('renderer')==root.get('renderer') and
                child.get('version','').startswith('4.5.0 NVIDIA '),'Missing distinct NVIDIA import context')
    require(root.get('vendor')=='NVIDIA Corporation' and 'NVIDIA GeForce RTX 5090 Laptop GPU' in root.get('renderer','') and
            root.get('version','').startswith('4.6.0 NVIDIA ') and
            all(caps.get(key)==root.get(key) for key in ('vendor','renderer','version')),'Wrong root GL adapter')
    require(caps.get('sameAdapterLuid') is True and caps.get('vendorId')==0x10de and
            caps.get('windowHidden') is True and caps.get('qemuFrontendVerified') is False,'Wrong adapter/frontend')
    for row in (caps,first,last):
        require(type(row.get('gpuUsageBytes')) is int and 0<row['gpuUsageBytes']<=1073741824,'GPU budget exceeded')
    require((opened.get('sourceWidth'),opened.get('sourceHeight'),opened.get('windowWidth'),opened.get('windowHeight'))==
            (3840,2160,1280,720) and all(opened.get(key) is True for key in
            ('originalNtHandlesClosed','namedObjects','independentFences')) and
            opened.get('sharedImportContext') is shared,'Wrong shared import topology')
    require(set(frames)=={f'interop-{kind}-{n}.rgba' for kind in ('source','window') for n in (1,2)},
            'Missing/extra complete pixels')
    proof=[]
    for number,row in enumerate((first,last),1):
        require(all(type(row.get(key)) is int and row[key]==number for key in
                    ('frame','producerFence','consumerFence','observedProducerFence','observedConsumerFence')) and
                row.get('independentFences') is True,'Unretired/mixed fence')
        require((row.get('sourceWidth'),row.get('sourceHeight'),row.get('windowWidth'),row.get('windowHeight'))==
                (3840,2160,1280,720) and row.get('cpuUpload') is False and row.get('diagnosticReadbacks')==2 and
                row.get('displayVerified') is False,'Unsupported transfer/display claim')
        captures=[]
        for kind,width,height,key in (('source',3840,2160,'sourceFnv1a'),('window',1280,720,'windowFnv1a')):
            checksum,digest=pattern_pixels(frames[f'interop-{kind}-{number}.rgba'],width,height,number)
            require(row.get(key)==f'{checksum:x}','Complete GPU pixel digest mismatch')
            captures.append(dict(kind=kind,bytes=width*height*4,sha256=digest,fnv1a=f'{checksum:016x}'))
        proof.append(dict(frame=number,producerFence=number,consumerFence=number,captures=captures))
    require(closed.get('frames')==2 and closed.get('sharedHandlesClosed')==3 and
            closed.get('sharedImportContext') is shared and all(closed.get(key) is True for key in
            ('sameAdapterLuid','namedObjects','independentFences','gpuOnlyTransfer','glObjectsReleased',
             'd3dObjectsReleased','sdlClosed')),'Incomplete cleanup')
    require(all(closed.get(key) is False for key in
                ('qemuFrontendVerified','guestFrameVerified','nearNativePerformanceVerified')) and
            source.get('qemuFrontendVerified') is False and source.get('guestFrameVerified') is False,
            'Unsupported product acceptance claim')
    return dict(hostInteropVerified=True,sharedImportContextVerified=shared,frames=proof,
                cpuUpload=False,diagnosticReadbacks=4,windowHidden=True,
                qemuFrontendVerified=False,guestFrameVerified=False,crossProcessImportVerified=False,
                physicalInputVerified=False,allFramesDisplayedVerified=False,nearNativePerformanceVerified=False)
