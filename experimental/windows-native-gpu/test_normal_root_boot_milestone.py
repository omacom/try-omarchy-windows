"""Retained real-image boots, missing-disk failure and independent evidence mutations."""
import copy
import gzip
import hashlib
import json
from pathlib import Path
import struct
import unittest
from make_normal_root_initramfs import package
from verify_normal_root_boot import validate


class NormalRootBoot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path=Path(__file__).resolve().parents[2]/'docs/evidence/NORMAL-OMARCHY-ROOT-BOOT-DEVELOPMENT-2026-10-16.json'
        cls.evidence=json.loads(cls.path.read_text())
        cls.runs={row['run']:row for row in cls.evidence['runs']}

    def audit(self,number=2,report=None,console=None):
        row=self.runs[number]
        return validate(row['report'] if report is None else report,row['console'].encode() if console is None else console,
            expected_source_sha256=self.evidence['expectedSourceSha256'],expected_source_path=self.evidence['expectedSourcePath'])

    def change(self,field,value):
        report=copy.deepcopy(self.runs[2]['report']);report[field]=value;return report

    def test_two_fresh_real_image_boots(self):
        results=[self.audit(number) for number in (1,2)]
        self.assertNotEqual(results[0]['bootId'],results[1]['bootId'])
        for result in results:
            self.assertTrue(result['normalRootBootVerified'])
            for key in ('normalGraphicsVerified','fullOmarchySceneVerified','desktopVerified',
                        'normalLauncherIntegrationVerified','multiGpuClientVerified','nearNativePerformanceVerified'):
                self.assertFalse(result[key])

    def test_missing_disk_control_rejected_before_root_switch(self):
        with self.assertRaises(ValueError):self.audit(3)
        self.assertIn('NORMAL_ROOT_BOOT_FAILURE operation=base-block-not-readonly',self.runs[3]['console'])
        self.assertNotIn('NORMAL_ROOT_SWITCH_ROOT',self.runs[3]['console'])
        self.assertFalse(self.runs[3]['report']['forcedStop'])

    def test_changed_or_foreign_source_rejected(self):
        for key in ('sourceSha256Before','sourceSha256After','qemuSha256','kernelSha256'):
            with self.assertRaises(ValueError):self.audit(report=self.change(key,'0'*64))

    def test_modified_console_rejected(self):
        with self.assertRaises(ValueError):self.audit(console=self.runs[2]['console'].encode()+b'changed')

    def test_writable_nodes_rejected(self):
        for node in (0,1):
            report=copy.deepcopy(self.runs[2]['report']);args=report['arguments']
            index=[i+1 for i,value in enumerate(args) if value=='-blockdev'][node]
            value=json.loads(args[index]);value['read-only']=False;args[index]=json.dumps(value)
            with self.assertRaises(ValueError):self.audit(report=report)

    def test_foreign_source_path_rejected(self):
        report=copy.deepcopy(self.runs[2]['report']);args=report['arguments'];index=args.index('-blockdev')+1
        value=json.loads(args[index]);value['filename']='D:/other.ext4';args[index]=json.dumps(value)
        with self.assertRaises(ValueError):self.audit(report=report)

    def test_missing_or_malformed_block_nodes_rejected(self):
        for value in ('[]','{','false'):
            report=copy.deepcopy(self.runs[2]['report']);index=report['arguments'].index('-blockdev')+1
            report['arguments'][index]=value
            with self.assertRaises(ValueError):self.audit(report=report)

    def test_forced_exit_and_false_gpu_claim_rejected(self):
        for key,value in (('forcedStop',True),('qemuExit',1),('qemuExit',False),('normalRootBootVerified',False),('graphicsVerified',True),('desktopVerified',True)):
            with self.subTest(key=key),self.assertRaises(ValueError):self.audit(report=self.change(key,value))

    def test_missing_readiness_even_with_matching_digest_rejected(self):
        data=self.runs[2]['console'].replace('NORMAL_ROOT_OS_READY','REMOVED').encode()
        report=self.change('consoleSha256',hashlib.sha256(data).hexdigest())
        with self.assertRaises(ValueError):self.audit(report=report,console=data)

    def test_evidence_remains_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'330f408ddeadb7e465e79f2680b1ea5b7e4d65a00ab96c5da1c9c4933c5a998d')

    @staticmethod
    def elf(type_):
        header=bytearray(64);header[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',header,16,type_,62)
        return bytes(header)

    def test_packager_layout_has_no_root_disk_or_vendor_runtime(self):
        module=self.elf(1)+b'vermagic=7.2.8-arch1-2 SMP\0'
        image,size=package(self.elf(2),module,'7.2.8-arch1-2')
        raw=gzip.decompress(image);self.assertEqual(len(raw),size)
        names=[];offset=0
        while offset<len(raw):
            self.assertEqual(raw[offset:offset+6],b'070701')
            fields=[int(raw[offset+6+i*8:offset+14+i*8],16) for i in range(13)]
            name=raw[offset+110:offset+110+fields[11]-1].decode();names.append(name)
            offset=(offset+110+fields[11]+3)&~3;offset=(offset+fields[6]+3)&~3
            if name=='TRAILER!!!':break
        self.assertEqual(names,['dev','dev/console','init','overlay.ko','TRAILER!!!'])

    def test_packager_rejects_wrong_architecture_release_and_bounds(self):
        init=self.elf(2);module=self.elf(1)+b'vermagic=7.2.8-arch1-2 SMP\0'
        arm=bytearray(init);struct.pack_into('<H',arm,18,183)
        big=bytearray(init);big[5]=2
        for executable,driver,release in ((b'bad',module,'7.2.8-arch1-2'),(init,b'bad','7.2.8-arch1-2'),
                (bytes(arm),module,'7.2.8-arch1-2'),(bytes(big),module,'7.2.8-arch1-2'),
                (init,module,'other'),(init,module,'../bad'),(init+b'x'*(2*1024*1024),module,'7.2.8-arch1-2')):
            with self.assertRaises(ValueError):package(executable,driver,release)


if __name__=='__main__':unittest.main()
