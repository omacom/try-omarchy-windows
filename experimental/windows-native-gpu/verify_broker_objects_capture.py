"""Audit broker-routed native resource FDs; recipient retention is not GPU import."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import require
from verify_broker_routing_capture import validate_broker_routing
from verify_resource_fd_capture import validate_resource_fds
from verify_mesa_fence_fd_capture import validate_mesa_fence_fds


def validate_broker_objects(report, guest, records, *, recipient=False):
    require(type(recipient) is bool, 'Recipient scope must be explicit')
    result=validate_broker_routing(report,guest,records,protocol=3)
    result.update(validate_resource_fds(report,guest,records,full_readback=True,
                                      transport='authenticated-broker',retained_recipient=recipient))
    lines=guest.splitlines()
    count=2 if recipient else 1
    summary=f'BROKER_OBJECT_SUMMARY exported={count} published={count} resolved=6 reaped={count} closed={count} retained=0'
    drain=f'BROKER_OBJECT_DRAIN producerExited=true retained={int(recipient)}'
    expected=[drain]
    if recipient:
        tickets=re.findall(r'^LINUX_BRIDGE kernelObjectExported=true kind=resource ticket=(\d+) cookie=(\d+)$',guest,re.M)
        expected.append(f'BROKER_OBJECT_CLOSE producerExited=true kind=2 ticket={tickets[1][0]} cookie={tickets[1][1]}')
    expected.append(summary)
    require([l for l in lines if l.startswith('BROKER_OBJECT')]==expected,
            'Extra, missing, failed or inconsistent capability-store receipts')
    native=report['disconnectCleanup']
    require(native.get('exportedSyncHandles')==native.get('closedSyncHandles')==native.get('liveSharedSyncHandles')==0,
            'Unexpected shared-sync workload')
    end=next(l for l in lines if l.startswith('BROKER_ROUTE_EXIT '))
    require(lines.index(drain)<lines.index(summary)<lines.index(end), 'Capability store retired after broker shutdown')
    result.update(brokerNativeResourceFdsVerified=True,brokerProtocol=3,
                  brokerRetainedReferenceCleanupVerified=recipient,
                  recipientFdBeyondProducerExitVerified=recipient,
                  crossProcessGpuResourceImportVerified=False,multiClientGpuRoutingVerified=False,
                  drmDeviceVerified=False,dmaBufVerified=False,gbmVerified=False,
                  desktopVerified=False,desktopStabilityVerified=False,nearNativePerformanceVerified=False)
    return result


def validate_broker_fences(report, guest, records):
    result=validate_broker_routing(report,guest,records,protocol=3)
    result.update(validate_mesa_fence_fds(report,guest,records,allocation_slots=256,transport='authenticated-broker'))
    lines=guest.splitlines()
    expected=['BROKER_OBJECT_DRAIN producerExited=true retained=0',
              'BROKER_OBJECT_SUMMARY exported=2 published=2 resolved=8 reaped=2 closed=2 retained=0',
              'BROKER_OBJECT recoverableErrors=4']
    require([l for l in lines if l.startswith('BROKER_OBJECT')]==expected,
            'Wrong shared-fence capability ownership or recoverable-error count')
    native=report['disconnectCleanup']
    require(native.get('exportedResourceHandles')==native.get('closedResourceHandles')==native.get('liveSharedResourceHandles')==0,
            'Unexpected resource-sharing workload')
    end=next(l for l in lines if l.startswith('BROKER_ROUTE_EXIT '))
    require(lines.index(expected[0])<lines.index(expected[1])<lines.index(expected[2])<lines.index(end),
            'Fence capability store retired after broker shutdown')
    result.update(brokerNativeFenceFdsVerified=True,brokerProtocol=3,
                  crossProcessGpuResourceImportVerified=False,multiClientGpuRoutingVerified=False,
                  drmDeviceVerified=False,dmaBufVerified=False,gbmVerified=False,
                  desktopVerified=False,desktopStabilityVerified=False,nearNativePerformanceVerified=False)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    scope=parser.add_mutually_exclusive_group()
    scope.add_argument('--recipient',action='store_true')
    scope.add_argument('--fences',action='store_true')
    args=parser.parse_args()
    guest=args.report.with_suffix('.log').read_text()
    records=[json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    report=json.loads(args.report.read_text())
    result=validate_broker_fences(report,guest,records) if args.fences else validate_broker_objects(report,guest,records,recipient=args.recipient)
    with args.output.open('x',encoding='utf8') as stream: json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__': main()
