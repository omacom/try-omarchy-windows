"""No-GPU controls for explicit routine trace selection and required diagnostics."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest

class BridgeLog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cxx=shutil.which('c++')
        if cls.cxx is None:raise unittest.SkipTest('A C++ compiler is required')
        cls.source=Path(__file__).with_name('guest_bridge_log_test.cpp')

    def test_default_verbose_and_quiet_preserve_arguments_errors_and_status(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-bridge-log-') as temporary:
            for flag in (None,0,1):
                with self.subTest(flag=flag):
                    binary=Path(temporary)/('log-'+str(flag))
                    command=[self.cxx,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-pthread']
                    if flag is not None:command+=['-DWDDM_BRIDGE_VERBOSE_IOCTL_TRACE='+str(flag)]
                    subprocess.run(command+[str(self.source),'-o',str(binary)],check=True,capture_output=True,text=True,timeout=30)
                    result=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=10)
                    self.assertIn('verbose='+str(1 if flag is None else flag),result.stdout)
                    self.assertIn('argumentEvaluated=true errorsAndStatusRetained=true',result.stdout)

    def test_unknown_trace_profile_fails_compilation(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-invalid-log-') as temporary:
            result=subprocess.run([self.cxx,'-std=c++17','-DWDDM_BRIDGE_VERBOSE_IOCTL_TRACE=2',str(self.source),'-o',str(Path(temporary)/'invalid')],capture_output=True,text=True,timeout=30)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('Ioctl trace selection must be zero or one',result.stderr)

    def test_format_mismatch_remains_a_compile_error(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-log-format-') as temporary:
            source=Path(temporary)/'bad.cpp';source.write_text('#include "guest_bridge_log.h"\nint main(){guest_bridge_log::diagnostic(stderr,"%d", "wrong");}\n',encoding='utf-8')
            result=subprocess.run([self.cxx,'-std=c++17','-Werror=format','-I'+str(self.source.parent),str(source),'-o',str(Path(temporary)/'invalid')],capture_output=True,text=True,timeout=30)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('format',result.stderr)

if __name__=='__main__':unittest.main(verbosity=2)
