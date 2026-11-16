"""CPU import contract parser controls only; not GPU evidence."""
from pathlib import Path
import tempfile
import unittest
from verify_dozen_memory_fd_import_contract import CASES, audit, validate_log


def fixture():
    return '\n'.join([f'DOZEN_IMPORT_FD_CONTRACT case={name} passed=true gpu=false' for name in CASES] +
        ['DOZEN_IMPORT_FD_CONTRACT_COMPLETE cases=8 failed=0 gpu=false']) + '\n'


class ImportFdEvidence(unittest.TestCase):
    def test_complete_case_set_passes(self): validate_log(fixture())

    def test_missing_duplicate_or_failed_case_is_rejected(self):
        value = fixture()
        for bad in (value.split('\n', 1)[1], value + value.splitlines()[0] + '\n',
            value.replace('passed=true', 'passed=false', 1), value.replace('gpu=false', 'gpu=true'), value.replace('cases=8', 'cases=7')):
            with self.subTest(bad=bad), self.assertRaises(ValueError): validate_log(bad)

    def test_compile_failure_cannot_be_counted_as_execution(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as name:
            folder = Path(name); (folder / 'build.log').write_text('compiler error')
            with self.assertRaises(FileNotFoundError): audit(folder, folder, folder)


if __name__ == '__main__': unittest.main()
