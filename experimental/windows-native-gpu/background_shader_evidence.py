"""Additional no-presentation and sampling checks around the existing 4K oracle."""
import re
from broker_runtime_evidence import broker_runtime_complete
from verify_4k_capture import require

FORBIDDEN = {'--driver-present-shared', '--driver-consume-shared-process',
             '--driver-sdl-relay', '--driver-window-input', '--driver-input-self-test',
             '--driver-rootfs-readonly', '--driver-coordinator', '--cpu-store-test'}


def validate_background_controls(source, guest, records, expected_reads):
    require(type(expected_reads) is int and expected_reads in (2, 10000), 'Unknown read selection')
    command = source.get('command')
    require(type(command) is list and command and all(type(arg) is str for arg in command), 'Invalid command')
    require(not FORBIDDEN.intersection(command), 'Foreground/input/disk option present')
    for option, expected in (('--runtime-workload', 'native'), ('--driver-cpu-slots', '128'),
                             ('--driver-fence-slots', '128')):
        require(command.count(option) == 1, 'Missing/duplicate ' + option)
        position = command.index(option)
        require(position + 1 < len(command) and command[position + 1] == expected, 'Wrong ' + option)
    require(source.get('sourceDiskAttached') is False and source.get('guiFlagsAbsent') is True and
            source.get('inputFlagsAbsent') is True, 'Background source contract differs')
    require(broker_runtime_complete(guest), 'Broker isolation/teardown not verified')
    require(not any(key.startswith(('nativeSdl', 'nativeSharedTexturePresent')) or
                    key == 'nativeInputChannelOpened' for row in records for key in row),
            'Presentation/input receipt in background run')
    prefix = r'LINUX_BRIDGE (?:pagingFenceRetired|allocationCpuMapped|[A-Za-z]+FenceMapped)=true direct=true loads='
    rows = re.findall(prefix + r'[^\r\n]*', guest)
    require(rows, 'No mapped-memory diagnostic receipts')
    counts = []
    for row in rows:
        match = re.match(prefix + r'([0-9]+) ', row)
        require(match is not None, 'Malformed direct-read receipt')
        count = int(match[1])
        require(count == expected_reads and match[1] == str(expected_reads),
                'Read selection differs from the pinned shim')
        counts.append(count)
    return dict(probeRecords=len(counts), diagnosticReadsPerRecord=expected_reads,
                diagnosticReadTotal=sum(counts), presentationRequested=False,
                inputRequested=False, sourceDiskAttached=False)
