"""Strict evidence for the opt-in typed texture metadata experiment."""
import json

BEGIN = 'GPU_NATIVE_METADATA_TEST_BEGIN width=130 height=73 format=28 flags=1'
END = 'GPU_NATIVE_METADATA_TEST_COMPLETE verified=true rounds=2'
DECLARED = 'LINUX_BRIDGE nativeTextureDeclared=true width=130 height=73 format=28 flags=1 guestRuntimePreserved=true'


def native_texture_runtime_bytes(guest, host, *, requested=False):
    """Return the validated native metadata size, or zero on any mismatch.

    The old workload must have no native metadata receipts/declarations.
    Metadata generation alone never proves resource import, pixels or display.
    """
    lines = guest.splitlines()
    markers = [line for line in lines if line.startswith('GPU_NATIVE_METADATA_')]
    declarations = [line for line in lines if line.startswith('LINUX_BRIDGE nativeTextureDeclared=')]
    try:
        records = [json.loads(line) for line in host.splitlines()
                   if line.startswith('{') and 'nativeTextureMetadata' in line]
    except (ValueError, TypeError):
        return 0
    if not requested:
        return 264 if not (markers or declarations or records) else 0
    if markers != [BEGIN, END] or declarations != [DECLARED] or len(records) != 2:
        return 0
    if not (lines.index(BEGIN) < lines.index(DECLARED) < lines.index(END)):
        return 0
    generated, applied = records
    if not isinstance(generated, dict) or not isinstance(applied, dict):
        return 0
    expected = dict(nativeTextureMetadataGenerated=True, width=130, height=73, format=28, flags=1,
                    allocationBytes=65536, allocationAlignment=65536, adapterNtstatus=0, deviceNtstatus=0,
                    queryNtstatus=0, openNtstatus=0, resourceDestroyedNtstatus=0, deviceDestroyedNtstatus=0,
                    adapterClosedNtstatus=0, sharedHandleClosed=True, prototypeReleased=True, privateBytesExported=False)
    expected_applied = dict(nativeTextureMetadataApplied=True, typedTexture=True, guestRuntimePreserved=True,
                            driverPrivateDataReused=True, ntstatus=0)
    if any(type(generated.get(k)) is not type(v) or generated[k] != v for k, v in expected.items()):
        return 0
    if any(type(applied.get(k)) is not type(v) or applied[k] != v for k, v in expected_applied.items()):
        return 0
    size = generated.get('runtimeBytes')
    if (type(size) is not int or not 0 < size <= 1024 or type(applied.get('nativeRuntimeBytes')) is not int or
            applied['nativeRuntimeBytes'] != size or type(applied.get('guestRuntimeBytes')) is not int or
            not 0 < applied['guestRuntimeBytes'] <= 1024):
        return 0
    return size
