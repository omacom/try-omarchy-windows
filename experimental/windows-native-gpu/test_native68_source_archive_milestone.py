"""Historical hardware proof stays bound to its actual compiled source bytes."""
import copy
import hashlib
import json
from pathlib import Path
import unittest

from native68_source_archive import ARCHIVE,NATIVE68_BINARY,native68_sources


class Native68Archive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        cls.receipt=json.loads((cls.repo/ARCHIVE).read_bytes())['nativeBuildReceipt']

    def test_exact_112_build_sources_and_original_mixed_line_endings(self):
        sources=native68_sources(self.repo,self.receipt)
        self.assertEqual(len(sources),112)
        self.assertEqual(self.receipt['bridgeSha256'],NATIVE68_BINARY)
        self.assertEqual(sum(b'\r\n' in raw for raw in sources.values()),37)
        for name,raw in sources.items():
            self.assertEqual(hashlib.sha256(raw).hexdigest(),self.receipt['sources'][name])

    def test_later_binary_source_revision_or_inventory_cannot_claim_old_proof(self):
        for field in ('bridgeSha256','repositoryRevision','buildScriptSha256'):
            receipt=dict(self.receipt);receipt[field]='0'*64
            with self.subTest(field=field),self.assertRaises(ValueError):
                native68_sources(self.repo,receipt)
        for name in self.receipt['sources']:
            receipt=copy.deepcopy(self.receipt);receipt['sources'][name]='0'*64
            with self.subTest(name=name),self.assertRaises(ValueError):
                native68_sources(self.repo,receipt)
        receipt=copy.deepcopy(self.receipt);receipt['sources'].pop(next(iter(receipt['sources'])))
        with self.assertRaises(ValueError):
            native68_sources(self.repo,receipt)


if __name__=='__main__':
    unittest.main()
