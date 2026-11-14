"""CPU receipt parser controls, not GPU evidence."""
import tempfile
from pathlib import Path
import unittest
from verify_dozen_memory_fd_contract import CASES, audit, validate_log


def fixture(profile):
    return '\n'.join(['DOZEN_FD_CONTRACT case=' + case + ' passed=' +
        ('true' if profile == 'patched' or case == 'unknown-type-rollback' else 'false') + ' gpu=false'
        for case in CASES] + ['DOZEN_FD_CONTRACT_COMPLETE cases=8 failed=' +
            ('0' if profile == 'patched' else '7') + ' gpu=false']) + '\n'


class FdContractEvidence(unittest.TestCase):
    def test_original_failures_and_fixed_cases_are_distinct(self):
        for profile, failed in (('original', 7), ('patched', 0)):
            self.assertEqual(validate_log(fixture(profile), profile)['failed'], failed)
            with self.assertRaises(ValueError):
                validate_log(fixture(profile), 'patched' if profile == 'original' else 'original')

    def test_partial_duplicate_and_wrong_scope_fail(self):
        good = fixture('patched')
        for bad in (good.split('\n', 1)[1], good + good.splitlines()[0] + '\n',
                    good.replace('gpu=false', 'gpu=true'), good.replace('cases=8', 'cases=7')):
            with self.subTest(bad=bad), self.assertRaises(ValueError): validate_log(bad, 'patched')

    def test_compile_failure_cannot_be_a_baseline(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            (root / 'build.log').write_text('compiler error')
            with self.assertRaises(FileNotFoundError): audit(root, 'original')


if __name__ == '__main__': unittest.main()
