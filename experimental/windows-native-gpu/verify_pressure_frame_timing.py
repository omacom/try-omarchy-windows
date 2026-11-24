"""Exact pressure-build timing; frame one includes the extra SDK diagnostic."""
import re
import statistics
from verify_hyprland_lifecycle_capture import require
from verify_sustained_virtio_desktop import ACTIVE_SOURCES as BASE_ACTIVE_SOURCES,STEADY_WINDOWS
GENERATOR='b3271daaffb03097d5998702b5670615ca6e8d975813cd15af84ab38a7f4ce38'
ACTIVE_SOURCES={'active_probe.c': '8aefc9a0765f1c3cc852bf7d07c13fe1321dfcdf70cd9bf0fb7eb09c8d798341', 'active_queue_probe.h': 'cfca45615d3b9b95e736b8b4215696b4571b4f526e12becbddeddd652378e8ea', 'gpu-terminal-scene.sh': 'a49179c43d0ea1a6c0327daf9e126dc76ccb4be8e9fd31fae74a99325f9bc23e', 'terminal_pulse.c': 'bd6db5881a7b3f3dd23edcd219cd68c4d43885e8588c202ef9d62470386130ec'}
PRESSURE_SOURCES={'d3d12_va_pressure_probe.cpp': '46fa09632d3fe6f4f15284e6bb2d831d01bec1a695f9b0ccaa9cab11aacd6e74', 'gpu_pressure_pattern.h': '20aca4f871c661a441f09b9885094cd3088660ee2723a1dac7065a513a38f964', 'build_gpu_va_pressure_probe.sh': '578736a69ef167df136f2d3abf14e078b562dfd60b47afa8dee25a3b105dd44b', 'pressure-frame-probe/generate.py': 'b3271daaffb03097d5998702b5670615ca6e8d975813cd15af84ab38a7f4ce38'}
PROBE_BINARY='632f98a9f479b2278b8a1bd4bb79b4430a1d84a30ebfe8b0e9880d85384ad31b'

def active_timing(guest,source):
    profile=source['manifest']['activeFrameProbe']
    require(profile['baselineGeneratorSha256']=='fafeb1f0324e21e2824b085d676c4ef48fe8f2c383d06b65b5b6fc2d4762252f' and
            profile['compiledFrameDefine']=='HYPRLAND_QUEUE_FRAMES=1800' and profile['renderingCodeChanged'] is False and
            profile['generatorSha256']==GENERATOR and profile['generatedSources']==ACTIVE_SOURCES and
            profile['preStreamD3D12Pressure'] is True and profile['baselineGeneratedSources']==BASE_ACTIVE_SOURCES and
            source['manifest']['pressureSources']==PRESSURE_SOURCES and
            source['manifest']['pressureBuildReceipt']['probeBinarySha256']==PROBE_BINARY and
            profile['pulseBinarySha256']=='73d0275e33d8a0241e00790dbd8da3adbfad1a22452e76f3490346a662a4bf09' and
            profile['targetUpdateHz']==60 and profile['notificationRepaints'] is False and
            profile['controllerObserverSleepUs']==2000 and profile['workloadDeadlineSeconds']==90 and
            profile['frameCount']==1800 and profile['baselineFixtureUnchanged'] is True and
            profile['cpuWallClockTimings'] is True and source['activeFrameProbeSourcesUnchanged'] is True and
            profile['inputImplementationChanged'] is profile['gpuTimestamps'] is
            profile['nearNativePerformanceVerified'] is profile['benchmarkAccepted'] is False,
            'Mixed/overclaimed active workload')
    pattern=(r'^HYPRLAND_ACTIVE_QUEUE_TIMING frame=(\d+) audit=([01]) fenceSetupNs=(\d+) '
             r'queueRoundTripNs=(\d+) totalNs=(\d+) queueAttempts=(\d+) monotonicNs=(\d+) '
             r'cpuWallClock=true gpuTimestamp=false$')
    rows=[dict(zip(('frame','audit','fenceSetupNs','queueRoundTripNs','totalNs','queueAttempts','monotonicNs'),map(int,row)))
          for row in re.findall(pattern,guest,re.M)]
    require(len(rows)==guest.count('HYPRLAND_ACTIVE_QUEUE_TIMING ')==1800,'Missing/invalid active timing')
    previous=0
    for index,row in enumerate(rows,1):
        require(row['frame']==index and row['audit']==int(index in (1,1800)) and row['queueAttempts']>=1 and
                row['fenceSetupNs']>0 and row['queueRoundTripNs']>0 and
                row['totalNs']>=row['fenceSetupNs']+row['queueRoundTripNs'] and
                row['monotonicNs']>previous,'Invalid CPU timing or frame order')
        previous=row['monotonicNs']
    duration=(rows[-1]['monotonicNs']-rows[0]['monotonicNs'])/1e9
    require(25<=duration<=90,'Stream did not sustain the preselected 25-second minimum within its deadline')
    steady=[row for row in rows if any(lo<=row['frame']<=hi for lo,hi in STEADY_WINDOWS)]
    deltas=[(rows[i]['monotonicNs']-rows[i-1]['monotonicNs'])/1e6 for i in range(1,len(rows))
            if any(lo<rows[i]['frame']<=hi for lo,hi in STEADY_WINDOWS)]
    return dict(rows=1800,streamDurationSeconds=duration,minimumSustainedSeconds=25,
                steadyRows=len(steady),steadyWindows=[list(w) for w in STEADY_WINDOWS],
                mediansMs={key:statistics.median(row[key]/1e6 for row in steady)
                           for key in ('fenceSetupNs','queueRoundTripNs','totalNs')},
                frameIntervalMedianMs=statistics.median(deltas),
                frameIntervalP95Ms=statistics.quantiles(deltas,n=100)[94],
                queueRetries=sum(row['queueAttempts']-1 for row in rows),
                firstFrameIncludesPreStreamDiagnostic=True,cpuWallClockOnly=True,gpuTimestamps=False,physicalInputLatencyVerified=False,
                physicalScanoutCadenceVerified=False,nearNativePerformanceVerified=False)
