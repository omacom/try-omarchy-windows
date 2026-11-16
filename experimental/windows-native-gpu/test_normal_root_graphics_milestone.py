"""Real-root NVIDIA evidence and retained desktop failures; no GPU-only desktop claim."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_normal_root_graphics import validate


class NormalRootGraphics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.path=cls.root/'docs/evidence/NORMAL-OMARCHY-GPU-DEVELOPMENT-2026-10-16.json'
        cls.evidence=json.loads(cls.path.read_text())
        cls.runs={row['run']:row for row in cls.evidence['runs']}
        for row in cls.runs.values():
            row['nativeRecords']=json.loads(zlib.decompress(base64.b64decode(row['nativeRecordsZlibBase64'],validate=True)))
            row['report']=json.loads(zlib.decompress(base64.b64decode(row['reportZlibBase64'],validate=True)))
        cls.guest={number:zlib.decompress(base64.b64decode(row['guestReceiptsZlibBase64'],validate=True)).decode()
                   for number,row in cls.runs.items()}
        cls.frames={number:{name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}
                    for number,row in cls.runs.items()}

    def audit(self,number=10,**changes):
        row=self.runs[number]
        arguments=dict(report=row['report'],guest=self.guest[number],records=row['nativeRecords'],
                       frames=self.frames[number],source=row['source'],expected_frames=row['source']['manifest']['expectedFrames'])
        arguments.update(changes)
        return validate(**arguments)

    def test_two_fresh_300_frame_runs_and_initial_12_frame_run(self):
        for number in (6,9,10):
            value=self.audit(number)
            self.assertTrue(value['realRootNvidiaCompositionVerified'])
            self.assertTrue(value['sourceImageUnchanged'])
            self.assertEqual(value['ordinaryRenderUid'],1000)
            self.assertEqual(value['scene']['queued']['framesSubmitted'],12 if number==6 else 300)
            for key in ('normalOmarchySessionVerified','applicationGpuAccelerationVerified','desktopVerified',
                        'normalLauncherIntegrationVerified','longRunStabilityVerified','compositor4kVerified','nearNativePerformanceVerified'):
                self.assertFalse(value[key])
        self.assertNotEqual(self.runs[9]['report']['ownedQemuControl'],self.runs[10]['report']['ownedQemuControl'])

    def test_all_failed_attempts_remain_rejected(self):
        for number in (1,2,3,4,5,7,8,11,12,13):
            with self.subTest(run=number),self.assertRaises(ValueError):self.audit(number)

    def test_interleaved_close_receipt_is_not_accepted(self):
        line='HYPRLAND_SCENE_CLOSED clientUnmapped=true childExitMarker=true\n'
        for text in (self.guest[10].replace(line,line.rstrip()+'LINUX_BRIDGE other\n'),self.guest[10]+line):
            with self.assertRaises(ValueError):self.audit(guest=text)

    def test_source_disk_changes_or_foreign_image_rejected(self):
        for key in ('sourceImageSha256Before','sourceImageSha256After','sourceImageUnchanged'):
            source=copy.deepcopy(self.runs[10]['source']);source[key]=False if key.endswith('Unchanged') else '0'*64
            with self.assertRaises(ValueError):self.audit(source=source)

    def test_writable_or_duplicate_owner_policy_rejected(self):
        for key in ('fileNodeReadonly','rawNodeReadonly','sourceWriteSharingDenied'):
            records=copy.deepcopy(self.runs[10]['nativeRecords'])
            next(row for row in records if 'ownedQemuReadonlyRootAttached' in row)[key]=False
            with self.assertRaises(ValueError):self.audit(records=records)
        records=copy.deepcopy(self.runs[10]['nativeRecords'])
        records.append(next(row for row in records if 'ownedQemuReadonlyRootAttached' in row))
        with self.assertRaises(ValueError):self.audit(records=records)

    def test_privileged_client_or_changed_memory_bound_rejected(self):
        source=copy.deepcopy(self.runs[10]['source']);source['manifest']['ordinaryUid']=0
        with self.assertRaises(ValueError):self.audit(source=source)
        records=copy.deepcopy(self.runs[10]['nativeRecords'])
        next(row for row in records if 'ownedQemuPid' in row)['guestMemoryMiB']=4096
        with self.assertRaises(ValueError):self.audit(records=records)

    def test_service_failure_and_missing_poweroff_rejected(self):
        for text in (self.guest[10].replace('serviceResult=success','serviceResult=timeout'),
                     self.guest[10].replace('reboot: Power down','REMOVED'),
                     self.guest[10]+'NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=6 coreDumped=false\n'):
            with self.assertRaises(ValueError):self.audit(guest=text)

    def test_deferred_cleanup_or_nonzero_shim_finalizer_rejected(self):
        report=copy.deepcopy(self.runs[10]['report']);report['ownedQemuControl']['fenceMappingsReleasedAfterVmExit']=1
        with self.assertRaises(ValueError):self.audit(report=report)
        guest=self.guest[10].replace('pagingFences=0 hwQueueFences=0','pagingFences=1 hwQueueFences=0',1)
        with self.assertRaises(ValueError):self.audit(guest=guest)

    def test_independent_expected_frames_and_input_hashes_required(self):
        with self.assertRaises(ValueError):self.audit(expected_frames=12)
        for key in ('initramfsSha256','driverBridgeSha256','kernelSha256','qemuSha256'):
            report=copy.deepcopy(self.runs[10]['report']);report[key]='0'*64
            with self.assertRaises(ValueError):self.audit(report=report)

    def test_full_capture_bytes_match_native_receipt(self):
        frames=self.frames[10].copy();raw=bytearray(frames['compositor-copy-2.raw']);raw[0]^=1
        frames['compositor-copy-2.raw']=bytes(raw)
        with self.assertRaises(ValueError):self.audit(frames=frames)

    def test_software_shell_appearance_never_claims_gpu_only_desktop(self):
        self.assertTrue(self.evidence['firstDesktopAppearanceCaptured'])
        self.assertFalse(self.evidence['fullDesktopGpuVerified'])
        self.assertFalse(self.evidence['desktop4kVerified'])
        self.assertIn('qtQuickSoftware=true compositorNvidia=true productAcceptance=false',self.guest[12])
        line=next(line for line in self.guest[12].splitlines() if line.startswith('OMARCHY_DESKTOP_LAYERS_HEX '))
        layers=json.loads(bytes.fromhex(line.split()[1]))['WDDM-1']['levels']
        self.assertEqual(layers['0'][0]['namespace'],'omarchy-background')
        self.assertEqual(layers['2'][0]['namespace'],'omarchy-bar')
        self.assertEqual(layers['0'][0]['pid'],layers['2'][0]['pid'])

    def test_evidence_is_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'30a9aea677ee8dffebf9bce0565062d41ccb2606d1c934731c63aacb84dfe7a7')


if __name__=='__main__':unittest.main()
