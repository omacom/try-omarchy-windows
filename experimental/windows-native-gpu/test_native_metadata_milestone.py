"""Synthetic evidence rejection tests, not hardware acceptance."""
import copy
import json
import unittest
from native_metadata_evidence import BEGIN, END, DECLARED, native_texture_runtime_bytes


class NativeMetadataTests(unittest.TestCase):
    def setUp(self):
        self.guest = '\n'.join((BEGIN, DECLARED, END))
        self.records = [dict(nativeTextureMetadataGenerated=True, width=130, height=73, format=28, flags=1,
            allocationBytes=65536, allocationAlignment=65536, runtimeBytes=280, adapterNtstatus=0, deviceNtstatus=0,
            queryNtstatus=0, openNtstatus=0, resourceDestroyedNtstatus=0, deviceDestroyedNtstatus=0,
            adapterClosedNtstatus=0, sharedHandleClosed=True, prototypeReleased=True, privateBytesExported=False),
            dict(nativeTextureMetadataApplied=True, typedTexture=True, guestRuntimeBytes=264, nativeRuntimeBytes=280,
                 guestRuntimePreserved=True, driverPrivateDataReused=True, ntstatus=0)]

    def check(self, records=None, guest=None, requested=True):
        return native_texture_runtime_bytes(self.guest if guest is None else guest,
            '\n'.join(json.dumps(r) for r in (self.records if records is None else records)), requested=requested)

    def test_explicit_workload_and_old_path(self):
        self.assertEqual(self.check(), 280)
        self.assertEqual(self.check(requested=False), 0)
        self.assertEqual(self.check(records=[], guest='', requested=False), 264)
        self.assertEqual(self.check(records=[], guest=''), 0)

    def test_missing_duplicate_reordered_receipts(self):
        for records in (self.records[:1], self.records[1:], self.records*2, self.records[::-1], [True, False]):
            self.assertEqual(self.check(records=records), 0)
        for guest in (BEGIN+'\n'+END, self.guest+'\n'+END, '\n'.join((DECLARED, BEGIN, END)), self.guest.replace('flags=1','flags=3')):
            self.assertEqual(self.check(guest=guest), 0)

    def test_every_required_receipt_field(self):
        for index, record in enumerate(self.records):
            for key, value in record.items():
                records=copy.deepcopy(self.records)
                del records[index][key]
                self.assertEqual(self.check(records=records), 0, key)
                records=copy.deepcopy(self.records)
                records[index][key]=int(value) if type(value) is bool else str(value)
                self.assertEqual(self.check(records=records), 0, key)

    def test_size_bounds_and_cross_check(self):
        for field, index in (('runtimeBytes',0), ('nativeRuntimeBytes',1), ('guestRuntimeBytes',1)):
            for invalid in (0,-1,1025,True,264.0):
                records=copy.deepcopy(self.records); records[index][field]=invalid
                self.assertEqual(self.check(records=records),0)
        records=copy.deepcopy(self.records); records[1]['nativeRuntimeBytes']=264
        self.assertEqual(self.check(records=records),0)


if __name__ == '__main__':
    unittest.main()
