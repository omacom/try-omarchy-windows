"""Audit the broker process boundary plus the unchanged, directly routed GPU probe."""
import argparse
import json
from pathlib import Path
from verify_4k_capture import require
from verify_resource_fd_capture import validate_resource_fds


def validate_broker_peer(report, guest, records):
    result = validate_resource_fds(report, guest, records, full_readback=True)
    lines = guest.splitlines()
    expected = (
        'BROKER_PEER_FINALIZER socketEof=true aliveDuringFinalizer=true exitViaPidfd=true uid=65534',
        'BROKER_PEER_THREAD_GROUP leaderZombie=true liveWorkerRetained=true',
        'BROKER_PEER_CONTRACT_PASS malformedCases=13 fdLeaks=0 independentPeers=true gpu=false',
        'BROKER_PEER_START contracts=true gpuRouting=false')
    observed = [line for line in lines if line.startswith('BROKER_PEER_')]
    require(observed == list(expected), 'Missing, duplicated, failed or changed broker process contract')
    require(lines.index(expected[-1]) < lines.index('KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false'),
            'Broker contract did not precede the genuine GPU regression')
    result.update(brokerPeerBoundaryVerified=True, brokerTestClientUid=65534,
                  brokerMalformedPacketCases=13, threadGroupExitVerified=True,
                  perMessageProcessCredentialsVerified=True, descriptorLeakChecksVerified=True,
                  multiClientGpuRoutingVerified=False, crossProcessGpuResourceImportVerified=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    result = validate_broker_peer(report, guest, records)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
