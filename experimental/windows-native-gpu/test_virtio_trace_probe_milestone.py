import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from virtio_trace import HEADER,decode_trace,merge_session


class VirtioTraceProbe(unittest.TestCase):
    def test_exact_bytes_checksum_and_explicit_session_merge(self):
        payload=b'complete GPU receipts\nincluding first failure\n'
        raw=HEADER+payload+b'WDDM_VIRTIO_TRACE_END bytes=46 fnv1a=daca8cc128dcd49a complete=true\n'
        self.assertEqual(decode_trace(raw)[0],payload)
        begin=b'NORMAL_ROOT_VIRTIO_TRACE_BEGIN port=vport0p2 namedIdentityVerified=true liveHostCapture=true inputChanged=false gpuProtocolChanged=false uid=1000\n'
        end=b'NORMAL_ROOT_VIRTIO_TRACE_END pumpExited=true complete=true inputChanged=false\n'
        uart=b'boot\n'+begin+b'ambient kernel event\n'+end+b'natural poweroff\n'
        text,proof=merge_session(uart,raw)
        self.assertIn(payload.decode(),text);self.assertIn('ambient kernel event\n',text)
        self.assertFalse(proof['ambientUartTotalOrderingVerified'])
        self.assertEqual(proof['payloadBytes'],46)

    def test_truncation_corruption_overflow_and_missing_boundary_rejected(self):
        payload=b'complete GPU receipts\nincluding first failure\n'
        raw=HEADER+payload+b'WDDM_VIRTIO_TRACE_END bytes=46 fnv1a=daca8cc128dcd49a complete=true\n'
        for bad in (raw[:-1],raw.replace(b'bytes=46',b'bytes=47'),raw.replace(b'first failure',b'first success'),raw.replace(b'complete=true',b'complete=false')):
            with self.assertRaises(ValueError):decode_trace(bad)
        with self.assertRaises(ValueError):merge_session(b'no session boundary\n',raw)

    def test_deterministic_derivation_preserves_original_and_rejects_source_drift(self):
        gpu=Path(__file__).resolve().parent
        spec=importlib.util.spec_from_file_location('trace_generator',gpu/'virtio-trace-probe/generate.py')
        generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
        before=(gpu/'normal_root_graphics_service.c').read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);a=generator.generate(gpu,root/'a');b=generator.generate(gpu,root/'b')
            self.assertEqual(a,b);self.assertEqual(hashlib.sha256(before).hexdigest(),a['baselineSupervisorSha256'])
            for name in a['generatedSources']:self.assertEqual((root/'a'/name).read_bytes(),(root/'b'/name).read_bytes())
            with self.assertRaises(FileExistsError):generator.generate(gpu,root/'a')
            other=root/'changed';other.mkdir();(other/'normal_root_graphics_service.c').write_bytes(before+b'\n')
            with self.assertRaises(ValueError):generator.generate(other,root/'bad')
            self.assertFalse((root/'bad').exists())
        self.assertEqual((gpu/'normal_root_graphics_service.c').read_bytes(),before)


if __name__=='__main__':unittest.main()
