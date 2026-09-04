from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("smoke-guest.py")
SPEC = importlib.util.spec_from_file_location("smoke_guest", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
smoke_guest = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke_guest)


class ParseFactsTests(unittest.TestCase):
    def test_ignores_echoed_placeholders_and_keeps_last_real_value(self) -> None:
        transcript = (
            b"printf 'TRYOMARCHY_FACT:yay:%s\\n' \"$(pacman -Q yay)\"\r\n"
            b"\x1b[?2004lTRYOMARCHY_FACT:yay:missing\r\n"
            b"TRYOMARCHY_FACT:sshd:inactive\r\n"
            b"TRYOMARCHY_FACT:yay:present\r\n"
        )
        self.assertEqual(smoke_guest.parse_facts(transcript), {"yay": "present", "sshd": "inactive"})

    def test_tolerates_bad_utf8_and_rejects_quoted_or_escaped_values(self) -> None:
        transcript = (
            b"\xffTRYOMARCHY_FACT:foreign:0\n"
            b"TRYOMARCHY_FACT:quoted:'present'\n"
            b"TRYOMARCHY_FACT:escaped:present\\later\n"
        )
        self.assertEqual(smoke_guest.parse_facts(transcript), {"foreign": "0"})


if __name__ == "__main__":
    unittest.main()
