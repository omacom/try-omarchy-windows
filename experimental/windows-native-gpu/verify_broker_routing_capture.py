"""Audit real single-client broker rendering, never a desktop or multi-client GPU."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import require
from verify_kernel_aperture_capture import validate_kernel_aperture
from verify_exit_lifetime_capture import validate_exit_lifetime


def validate_broker_routing(report, guest, records, *, finalizer=False, protocol=2):
    require(type(protocol) is int and protocol in (2,3), 'Broker protocol selection must be explicit')
    audit = validate_exit_lifetime if finalizer else validate_kernel_aperture
    result = audit(report, guest, records, allocation_slots=256, transport='authenticated-broker')
    lines = guest.splitlines()
    client = 'BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied'
    require(lines.count(client) == 1, 'Missing ordinary rendering-client privilege proof')
    start = [l for l in lines if l.startswith('BROKER_ROUTE_START ')]
    require(len(start) == 1 and re.fullmatch(rf'BROKER_ROUTE_START protocol={protocol} clientPid=[1-9]\d* clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false',start[0]),
            'Missing exact broker peer and worker binding')
    end = [l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')]
    require(len(end) == 1, 'Missing broker process-exit receipt')
    counts = re.fullmatch(r'BROKER_ROUTE_EXIT processExited=true retainedViews=(\d+) nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false',end[0])
    require(counts is not None, 'Broker rejection, lifetime or scope failure')
    retained,calls,created,released = map(int,counts.groups())
    routes = [l for l in lines if l.startswith('BROKER_ROUTE')]
    require(routes == [start[0],end[0],'BROKER_ROUTE_REAP exit=0'], 'Extra, failed or duplicate broker lifecycle receipts')
    stopped = [l for l in lines if l.startswith('LINUX_BRIDGE processExitViewsRetained=')]
    require(len(stopped) == 1, 'Missing client retained-view receipt')
    views = re.fullmatch(r'LINUX_BRIDGE processExitViewsRetained=true pagingFences=(\d+) hwQueueFences=(\d+) syncFences=(\d+) allocationViews=(\d+) release=kernel-process-exit',stopped[0])
    require(views is not None, 'Client CPU-view lifetime changed')
    vm=report['ownedQemuControl']
    require(0 < retained == sum(map(int,views.groups())) == created-released ==
            vm['fenceMappingsReleasedAfterVmExit']+vm['allocationMappingsReleasedAfterVmExit'],
            'Broker/client/native retained mappings disagree')
    require(created == result['kernelFenceLeases']+result['kernelAllocationLeases'] and
            released == lines.count('LINUX_BRIDGE kernelApertureRevoked=true') and
            result['retiredNativeCommands'] <= calls <= 10000, 'Broker/native lease or request accounting disagrees')
    require(lines.index(client) < lines.index(start[0]) < lines.index(stopped[0]) < lines.index(end[0]) <
            lines.index('BROKER_ROUTE_REAP exit=0') < lines.index('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true') <
            lines.index('BRIDGE_RUNTIME_EXIT=0'), 'Wrong process/broker/kernel cleanup order')
    peer = [l for l in lines if l.startswith('BROKER_PEER_')]
    expected = ['BROKER_PEER_FINALIZER socketEof=true aliveDuringFinalizer=true exitViaPidfd=true uid=65534',
                'BROKER_PEER_THREAD_GROUP leaderZombie=true liveWorkerRetained=true',
                'BROKER_PEER_CONTRACT_PASS malformedCases=14 fdLeaks=0 independentPeers=true gpu=false',
                'BROKER_PEER_START contracts=true gpuRouting=false']
    require(peer == expected and lines.index(expected[-1]) <
            lines.index('KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false') < lines.index(client),
            'Broker peer prerequisites changed')
    if not finalizer:
        require(not any(l.startswith('EXIT_LIFETIME_') for l in lines), 'Unexpected finalizer workload')
    else:
        require(lines.index('EXIT_LIFETIME_FINALIZER_COMPLETE observed=57 mappingLive=true') < lines.index(end[0]),
                'Broker retired before the real native finalizer finished')
    result.update(brokerRoutingVerified=True, unprivilegedGraphicsClientVerified=True, clientUid=65534,
                  nativeWorkerClients=1, brokerNativeCalls=calls, brokerViewsCreated=created,
                  brokerViewsExplicitlyReleased=released, brokerViewsRetainedUntilProcessExit=retained,
                  brokerNativeFinalizerVerified=finalizer, multiClientGpuRoutingVerified=False,
                  crossProcessGpuResourceImportVerified=False, gbmVerified=False, desktopStabilityVerified=False)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--finalizer',action='store_true')
    args=parser.parse_args()
    guest=args.report.with_suffix('.log').read_text()
    records=[json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    result=validate_broker_routing(json.loads(args.report.read_text()),guest,records,finalizer=args.finalizer)
    with args.output.open('x',encoding='utf8') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__': main()
