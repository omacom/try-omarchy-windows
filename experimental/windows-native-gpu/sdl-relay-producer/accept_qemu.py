"""Owned, disk-free WHPX/QEMU SDL relay control; no guest desktop claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import tarfile
import threading


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Producer:
    def __init__(self,binary,session,generation,output,bgra=False):
        self.records=[];self.messages=queue.Queue()
        self.log=(output/'producer.stdout.log').open('x',encoding='utf-8')
        self.errors=(output/'producer.stderr.log').open('x',encoding='utf-8')
        self.command=[str(binary),session,str(generation)]+(['--bgra'] if bgra else [])
        self.process=subprocess.Popen(self.command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                      stderr=self.errors,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
        self.reader=threading.Thread(target=self.read,daemon=True);self.reader.start()

    def read(self):
        try:
            while line:=self.process.stdout.readline(65537):
                self.log.write(line);self.log.flush()
                if len(line)>65536:
                    raise ValueError('Oversized producer record')
                row=json.loads(line);self.records.append(row);self.messages.put(row)
        except BaseException as error:
            self.messages.put(dict(readerError=repr(error)))

    def receive(self,key):
        row=self.messages.get(timeout=15)
        if row.get(key) is not True:
            raise ValueError('Unexpected producer record: '+repr(row))
        return row

    def send(self,line):
        self.process.stdin.write(line+'\n');self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:self.process.kill()
        self.process.wait(timeout=10);self.reader.join(timeout=2)
        self.process.stdin.close();self.process.stdout.close();self.log.close();self.errors.close()


class Qmp:
    def __init__(self,connection,records):
        self.connection=connection;connection.settimeout(15)
        self.stream=connection.makefile('rwb');self.records=records;self.counter=0
        if 'QMP' not in self.read():raise ValueError('Missing QMP greeting')
        self.call('qmp_capabilities')

    def read(self):
        raw=self.stream.readline(1024*1024)
        if not raw or len(raw)>=1024*1024:raise ValueError('Disconnected/oversized QMP record')
        row=json.loads(raw);self.records.append(dict(received=row));return row

    def call(self,name,arguments=None,error=False):
        self.counter+=1;request=dict(execute=name,id=self.counter)
        if arguments is not None:request['arguments']=arguments
        self.records.append(dict(sent=request));self.stream.write(json.dumps(request).encode()+b'\n');self.stream.flush()
        for unused in range(64):
            row=self.read()
            if row.get('id')!=self.counter:continue
            if ('error' in row) is not error:raise ValueError('Unexpected QMP result: '+repr(row))
            return row['error' if error else 'return']
        raise ValueError('Too many unrelated QMP events')

    def state(self):
        return json.loads(self.call('qom-get',dict(path='/machine/peripheral/relay',property='relay-state')))

    def command(self,value,error=False):
        return self.call('qom-set',dict(path='/machine/peripheral/relay',property='relay-command',value=value),error=error)

    def close(self):
        self.stream.close();self.connection.close()


def accept(candidate,build,output,artifact_receipt,frames=2,tcg=False,bgra=False):
    candidate,build,output=(path.resolve() for path in (candidate,build,output))
    if not 2<=frames<=300:raise ValueError('Control frame count must be in 2..300')
    output.mkdir()
    audit=output/'gpu-buffers';audit.mkdir()
    repo=Path(__file__).resolve().parents[3]
    binary=candidate/'bin/qemu-system-x86_64.exe'
    producer_binary=build/'d3d12-named-producer.exe'
    artifact=json.loads(artifact_receipt.read_text())
    for name,digest in artifact['files'].items():
        if sha(candidate/name)!=digest:raise ValueError('Candidate differs from its verified artifact receipt')
    receipt=json.loads((build/'receipt.json').read_text(encoding='utf-8-sig'))
    if sha(Path(__file__).with_name('d3d12_named_producer.cpp'))!=receipt['sourceSha256'] or \
       sha(Path(__file__).with_name('build.ps1'))!=receipt['buildScriptSha256'] or \
       sha(producer_binary)!=receipt['binarySha256']:
        raise ValueError('Producer build/source changed')
    provenance=json.loads((candidate/'sdl-candidate-provenance.json').read_text())
    archive=candidate/'qemu-corresponding-source.tar.gz'
    compiled={}
    with tarfile.open(archive,'r:gz') as bundle:
        for name in [*('hw/display/'+name for name in provenance['graphicsRelaySources']),
                     'include/ui/console.h','ui/console.c','ui/sdl2-gl.c']:
            raw=bundle.extractfile('source/'+name).read()
            digest=hashlib.sha256(raw).hexdigest()
            if name.startswith('hw/display/') and digest!=provenance['graphicsRelaySources'][Path(name).name]:
                raise ValueError('Relay source differs from its compiled artifact manifest')
            compiled[name]=dict(sha256=digest,utf8=raw.decode())
    header=(candidate/'config-host.h').read_text()
    if not all(re.search(r'^#define '+key+r'\s*$',header,re.M) for key in ('CONFIG_OPENGL','CONFIG_SDL')):
        raise ValueError('QEMU lacks SDL/OpenGL')
    frozen_paths=[Path(__file__),
                  Path(__file__).with_name('d3d12_named_producer.cpp'),Path(__file__).with_name('build.ps1')]
    frozen={str(path.relative_to(repo)).replace('\\','/'):sha(path) for path in frozen_paths}
    qemu_hash=sha(binary);session=os.urandom(16).hex();generation=1
    source=dict(schema=1,session=session,generation=generation,frames=frames,
                qemuSha256=qemu_hash,producerReceipt=receipt,sourceImageUsed=False,requestedFormat='BGRA8' if bgra else 'RGBA8',
                qemuProvenance=provenance,runnerSha256=sha(Path(__file__)),frozenSources=frozen,
                artifactReceipt=artifact,compiledSources=compiled,
                physicalInputVerified=False,physicalScanoutVerified=False,guestGpuScanoutVerified=False,
                nearNativePerformanceVerified=False)
    result=dict(schema=1,passed=False,naturalQemuExit=False,naturalProducerExit=False,
                states=[],qmp=[],negativeControls=[],physicalInputVerified=False,
                physicalScanoutVerified=False,guestGpuScanoutVerified=False,nearNativePerformanceVerified=False)
    producer=None;process=None;qmp=None;listener=None
    try:
        producer=Producer(producer_binary,session,generation,output,bgra)
        ready=producer.receive('namedProducerReady')
        if ready['vendorId']!=0x10de or not ready['independentFences'] or \
           not ready['nonInheritableExports'] or (ready['width'],ready['height'])!=(3840,2160) or \
           ready['format']!=source['requestedFormat']:
            raise ValueError('Wrong NVIDIA resource topology')
        source['producerReady']=ready
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(15)
        name='Try-Omarchy-SDL-Relay-'+session
        device=('wddm-sdl-relay,id=relay,session='+session+',generation=1,adapter-luid='+ready['adapterLuid']+
                ',audit-directory='+str(audit).replace(',',',,'))
        if provenance.get('graphicsRelayProtocolVersion',1)>=2:
            device+=',source-format='+('bgra8' if bgra else 'rgba8')
        command=[str(binary),'-L',str(candidate/'share/qemu'),'-name',name,
                 '-machine','q35,accel='+('tcg' if tcg else 'whpx,kernel-irqchip=off'),
                 '-m','256','-S','-nodefaults','-vga','none','-device',device,
                 '-device','virtio-keyboard-pci','-device','virtio-tablet-pci',
                 '-display','sdl,gl=on,show-cursor=off,window-close=off',
                 '-qmp','tcp:127.0.0.1:'+str(listener.getsockname()[1])]
        source['qemuCommand']=command;source['producerCommand']=producer.command
        startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
        with (output/'qemu.stdout.log').open('x') as stdout,(output/'qemu.stderr.log').open('x') as stderr:
            process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,
                                     startupinfo=startup,creationflags=subprocess.CREATE_NO_WINDOW)
        connection,_=listener.accept();qmp=Qmp(connection,result['qmp'])
        if qmp.call('query-name')['name']!=name:raise ValueError('QMP process owner mismatch')
        if qmp.call('query-status')['running']:raise ValueError('Disk-free VM unexpectedly running')
        options=qmp.call('query-display-options')
        if options.get('type')!='sdl' or options.get('gl')!='on':raise ValueError('Wrong actual display options')
        result['displayOptions']=options
        mice=qmp.call('query-mice')
        if not any(row['absolute'] and 'tablet' in row['name'].lower() for row in mice):
            raise ValueError('Official virtio tablet absent')
        for text in ('present:1:1','attach:2','attach:01'):
            result['negativeControls'].append(dict(command=text,error=qmp.command(text,error=True)))
        qmp.command('attach:1');state=qmp.state();result['states'].append(state)
        if not state['attached'] or state['poisoned'] or not state['rootAdapterVerified'] or \
           state['liveImportedObjects']!=4 or not state['importContextLive'] or not state['localScanoutLive'] or \
           state['adapterLuid']!=ready['adapterLuid'] or \
           state['vendor']!='NVIDIA Corporation' or 'NVIDIA' not in state['renderer']:
            raise ValueError('QEMU root context is not the matching hardware NVIDIA adapter')
        if provenance.get('graphicsRelayProtocolVersion',1)>=2 and (
                state['sourceFormat']!=source['requestedFormat'].lower() or state['channelProgramLive'] is not bgra):
            raise ValueError('Wrong GPU channel-conversion path')
        for text in ('attach:1','present:1:2'):
            result['negativeControls'].append(dict(command=text,error=qmp.command(text,error=True)))
        producer.send('close-handles')
        if producer.receive('namedProducerExportsClosed')['handles']!=3:raise ValueError('Export handles retained')
        for frame in range(1,frames+1):
            producer.send('frame:'+str(frame))
            queued=producer.receive('namedProducerFrameQueued')
            if queued['frame']!=frame or queued['producerTarget']!=frame or queued['cpuTextureUpload']:
                raise ValueError('Wrong producer frame')
            mode='audit' if frame in (1,frames) else 'present'
            qmp.command(mode+':1:'+str(frame))
            retired=producer.receive('namedProducerConsumerRetired')
            if (retired['frame'],retired['producerFence'],retired['consumerFence'])!=(frame,frame,frame) or \
               not 0<retired['gpuUsageBytes']<=1073741824:
                raise ValueError('Independent GPU fences did not retire or producer budget exceeded')
            state=qmp.state();result['states'].append(state)
            if state['lastQueuedFrame']!=frame or state['poisoned'] or \
               state['auditFrames']!=(2 if frame==frames else 1):
                raise ValueError('QEMU frame/audit ownership mismatch')
            if provenance.get('graphicsRelayProtocolVersion',1)>=2 and state['convertedFrames']!=(frame if bgra else 0):
                raise ValueError('Missing/extra GPU channel-conversion dispatch')
        qmp.command('detach:1');state=qmp.state();result['states'].append(state)
        if state['attached'] or state['poisoned'] or state['liveImportedObjects'] or \
           state['importContextLive'] or state['localScanoutLive']:
            raise ValueError('GPU relay did not detach cleanly')
        if provenance.get('graphicsRelayProtocolVersion',1)>=2 and state['channelProgramLive']:
            raise ValueError('GPU conversion program remains live after detach')
        result['negativeControls'].append(dict(command='attach:1',error=qmp.command('attach:1',error=True)))
        producer.send('close');done=producer.receive('namedProducerComplete')
        if not done['gpuObjectsReleased'] or done['frames']!=frames or done['exportsClosed']!=3:
            raise ValueError('Producer GPU ownership not reclaimed')
        if producer.process.wait(timeout=10)!=0:raise ValueError('Producer exit failed')
        result['naturalProducerExit']=True
        qmp.call('quit')
        if process.wait(timeout=10)!=0:raise ValueError('QEMU natural shutdown failed')
        result['naturalQemuExit']=True;result['passed']=True
    except BaseException as error:
        result['error']=type(error).__name__+': '+str(error)
        if qmp:
            try:result['failureState']=qmp.state()
            except BaseException:pass
        raise
    finally:
        if process:
            if process.poll() is None:process.kill()
            result['qemuExitCode']=process.wait(timeout=10)
        if qmp:qmp.close()
        if listener:listener.close()
        if producer:
            producer.close();result['producerExitCode']=producer.process.returncode
            result['producerRecords']=producer.records
        source['artifactUnchanged']=all(sha(candidate/name)==digest for name,digest in artifact['files'].items())
        source['binariesUnchanged']=sha(binary)==qemu_hash and sha(producer_binary)==receipt['binarySha256']
        source['sourcesUnchanged']=all(sha(repo/name)==digest for name,digest in frozen.items())
        if not source['artifactUnchanged'] or not source['binariesUnchanged'] or not source['sourcesUnchanged']:
            result['passed']=False
            result['error']='Source or binary changed during the owned GPU control'
        for name,value in (('source.json',source),('result.json',result)):
            with (output/name).open('x',encoding='utf-8',newline='\n') as file:
                json.dump(value,file,indent=2);file.write('\n')
    if not result['passed']:
        raise ValueError(result.get('error','QEMU GPU control failed'))
    print('Owned cross-process QEMU SDL GPU control complete; independent pixel auditing still required')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--producer-build',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--artifact-receipt',type=Path,required=True,help='Receipt from the digest-verified artifact download')
    parser.add_argument('--frames',type=int,default=2)
    parser.add_argument('--tcg',action='store_true')
    parser.add_argument('--bgra',action='store_true',help='Probe the BGRA format used by real compositor buffers; independent pixels must still pass')
    args=parser.parse_args()
    accept(args.candidate,args.producer_build,args.output,args.artifact_receipt,args.frames,args.tcg,args.bgra)
