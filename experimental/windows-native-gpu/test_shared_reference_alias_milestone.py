"""Keep native NT alias evidence separate from guest actor/shared-FD integration."""
import base64
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_shared_reference_aliases import validate
from verify_4k_presentation_budget import validate as compositor


class SharedReferenceAliases(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.value=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/NATIVE-SHARED-REFERENCE-ALIASES-2026-10-16.json').read_text())
        unpack=lambda key:zlib.decompress(base64.b64decode(cls.value[key],validate=True))
        cls.resource=unpack('resourceLogZlibBase64');cls.sync=unpack('syncLogZlibBase64')

    def test_actual_nvidia_resource_and_fence_lifetime_pass_scoped_audit(self):
        proof=validate(self.resource.decode(),self.sync.decode());self.assertEqual(proof,self.value['nativeAudit'])
        self.assertTrue(proof['nativeResourceAliasLifetimeVerified']);self.assertTrue(proof['nativeFenceAliasLifetimeVerified'])
        self.assertEqual(proof['pixelsVerified'],9490);self.assertEqual(proof['gpuSignalsVerified'],16)
        for key in ('guestSharedReferenceAliasVerified','secondGuestGpuClientVerified','quickshellGpuVerified','desktopVerified','nearNativePerformanceVerified'):
            self.assertFalse(proof[key])

    def test_independent_probe_and_log_provenance_is_fixed(self):
        expected={
            'sync-sharing-probe':('bd4420f19ff16a0ee7d21539e35c98fdff1789151623b8784e559a48e044f641','3bfdb6679be285f79e880ef16e26408d6c5505a7a708d93f45d402e76e5923d4',self.sync),
            'shared-reference-alias-probe':('820be3af09c66e66482b4ccf8571e5ecb9920cc7f9f58ffa6ff214fe632cff07','7a552bc71ef0c093f90d466b069213293dd1cd1dfd7f08de5be1afa3e9d0c29e',self.resource)}
        for row in self.value['nativeProbes']['probes']:
            binary,digest,log=expected[row['probe']];self.assertEqual(row['exeSha256'],binary);self.assertEqual(row['logSha256'],digest)
            self.assertEqual(hashlib.sha256(log).hexdigest(),digest);self.assertEqual(row['exit'],0)
        self.assertEqual(len(self.value['nativeProbes']['probes']),2)

    def test_software_wrong_resource_lifetime_and_incomplete_cleanup_are_rejected(self):
        for old,new in (('vendor=4318','vendor=5140'),('pixels=9490','pixels=9489'),('gpuClear=true','gpuClear=false'),
            ('originalResourceReleasedBeforeImport=true','originalResourceReleasedBeforeImport=false'),
            ('aliasNtClosedBeforeCopy=true','aliasNtClosedBeforeCopy=false'),('cleanup=true','cleanup=false'),
            ('cpuUpload=false','cpuUpload=true'),('guest=false','guest=true')):
            with self.subTest(old=old),self.assertRaises(ValueError):validate(self.resource.decode().replace(old,new),self.sync.decode())

    def test_missing_unretired_or_wrongly_ordered_gpu_signals_are_rejected(self):
        text=self.sync.decode().replace('\r\n','\n')
        line=next(line for line in text.splitlines(True) if line.startswith('NATIVE_SYNC_GPU_RETIRED target=9 '))
        for bad in (text.replace(line,''),text+line,text.replace(line,line.replace('recipient=9','recipient=0')),
            text.replace(line,line.replace('d3d12=9','d3d12=18446744073709551615')),
            text.replace(line,line.replace('sourceAlive=0','sourceAlive=1')),
            text.replace('originalNtClosedBeforeImport=true','originalNtClosedBeforeImport=false'),
            text.replace('NATIVE_SYNC_NT stage=DestroySource status=00000000\n','')):
            with self.assertRaises(ValueError):validate(self.resource.decode(),bad)

    def test_actual_session_alias_controls_use_mock_native_calls(self):
        logs=self.value['validationLogs'];decode=lambda name:zlib.decompress(base64.b64decode(logs[name],validate=True)).decode()
        controls=decode('actor-alias-controls-v1.log')
        self.assertIn('ACTOR_RESOURCE_ALIAS physicalAdapterChecked=true negotiatedOnly=true independentNativeReference=true donorSessionDestroyed=true recipientImport=true malformedRepliesRejected=true quotasPreserved=true native=mock gpu=false',controls)
        self.assertIn('ACTOR_SYNC_ALIAS physicalAdapterChecked=true independentNativeReference=true donorSessionDestroyed=true recipientImport=true native=mock gpu=false',controls)
        self.assertIn('PASS: 105 native capacity/control rejection cases',decode('alias-default-cli.log'))

    def test_new_owner_preserves_clean_4k_single_client_regression(self):
        row=self.value['regression'];unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
        records=json.loads(unpack('nativeRecordsZlibBase64'))
        frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}
        report=json.loads(unpack('reportZlibBase64'));guest=unpack('guestReceiptsZlibBase64').decode().replace('\r\n','\n')
        proof=compositor(report,guest,records,frames,row['source']);self.assertEqual(proof,row['audit'])
        self.assertEqual(report['driverBridgeSha256'],'d2db1e9260950b6185ed9c575e3cdfdd15a3540b76202515ac4f3fb63e9fd156')
        self.assertTrue(proof['clean4kCompositorShutdownVerified']);self.assertFalse(proof['quickshellGpuVerified'])
        self.assertFalse(any('nativeSharedReferenceAliased' in r for r in records))


if __name__=='__main__':unittest.main()
