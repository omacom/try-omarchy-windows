"""Check the host's budget against QEMU's actual retirement wait, without execution."""
from pathlib import Path
import re
import unittest


class DeadlineContractTests(unittest.TestCase):
    def test_outer_graphics_budget_contains_the_qemu_child_wait(self):
        here = Path(__file__).resolve().parent
        host = (here / 'qmp_deadline_budget.h').read_text(encoding='utf-8')
        relay = (here.parent / 'qemu/wddm-sdl-relay.c').read_text(encoding='utf-8')
        group = (here / 'stopped_group_retirement_poll.h').read_text(encoding='utf-8')
        port = (here / 'guest_broker_server.h').read_text(encoding='utf-8').split('class Port {', 1)[1].split('\n};', 1)[0]

        def one(pattern, source):
            values = re.findall(pattern, source)
            self.assertEqual(len(values), 1, 'Expected one pinned deadline definition')
            return int(values[0])

        child_ns = one(r'glClientWaitSync\(retired, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_C\((\d+)\)\)', relay)
        child_ms = one(r'SdlRetirementFenceTimeoutMs = (\d+);', host)
        allowance_ms = one(r'SdlCommandReplyAllowanceMs = (\d+);', host)
        control_ms = one(r'ControlCommandTimeoutMs = (\d+);', host)
        self.assertEqual(child_ns, child_ms * 1_000_000)
        self.assertEqual(control_ms, 5000)
        self.assertGreater(allowance_ms, 0)
        group_ms = one(r'StoppedGroupAdmissionTimeoutMs = (\d+);', group)
        port_ms = one(r'deadline=std::chrono::steady_clock::now\(\)\+std::chrono::seconds\((\d+)\);', port) * 1000
        self.assertLess(child_ms + allowance_ms, group_ms)
        self.assertLess(group_ms, port_ms)
        self.assertGreater(child_ms + allowance_ms, child_ms)


if __name__ == '__main__':
    unittest.main()
