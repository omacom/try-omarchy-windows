import unittest
from unittest.mock import patch

import probe


class CapabilityTests(unittest.TestCase):
    def test_software_fallback_is_not_hardware_acceptance(self):
        for renderer in ('llvmpipe (LLVM)', 'lavapipe', 'SwiftShader Device', ''):
            result = {'status': 'completed', 'exitCode': 0,
                      'output': 'GPU_ADAPTER=' + renderer + '\nGPU_3D_READBACK_PASSED'}
            self.assertEqual(probe.classify_workload(
                result, 'GPU_3D_READBACK_PASSED', True)['status'], 'failed')

    def test_renderer_detection_does_not_require_a_vendor_or_model(self):
        for renderer in ('D3D12 (AMD Radeon)', 'D3D12 (Intel Arc)',
                         'D3D12 (NVIDIA GeForce)', 'virgl (Other adapter)'):
            result = {'status': 'completed', 'exitCode': 0,
                      'output': 'Accelerated: yes\nOpenGL renderer string: ' + renderer}
            self.assertEqual(probe.classify_gl(result)['status'], 'available')

    def test_exit_status_and_completion_marker_are_both_required(self):
        for code, output in ((1, 'GPU_RENDER_PASSED=CUDA'), (0, 'GPU_DEVICES=[]')):
            result = {'status': 'completed', 'exitCode': code, 'output': output}
            self.assertEqual(probe.classify_workload(result, 'GPU_RENDER_PASSED=CUDA')['status'], 'failed')

    def test_unavailable_and_timeout_remain_distinct(self):
        for state in ('unavailable', 'timeout'):
            result = {'status': state}
            self.assertEqual(probe.classify_gl(result)['status'], state)
            self.assertEqual(probe.classify_workload(result, 'passed')['status'], state)

    @patch('probe.shutil.which', return_value=None)
    def test_missing_tool_is_reported_without_launch(self, _which):
        self.assertEqual(probe.run_command(['missing'])['status'], 'unavailable')


if __name__ == '__main__':
    unittest.main()
