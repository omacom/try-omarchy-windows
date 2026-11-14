"""Real preloaded processes: ordinary FDs stay idle; explicit GPU failures stay visible."""
from pathlib import Path
import argparse,os,shutil,subprocess,tempfile,unittest

class IdlePreload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='omarchy-idle-preload-')
        cls.probe=Path(cls.temporary.name)/'probe'
        subprocess.run([shutil.which('cc') or 'cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',str(Path(__file__).with_name('idle_preload_probe.c')),'-o',str(cls.probe)],check=True,capture_output=True,timeout=30)
    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()
    def run_probe(self,mode):
        env={k:v for k,v in os.environ.items() if not k.startswith('WDDM_') and k!='LD_PRELOAD'}
        env['LD_PRELOAD']=str(SHIM.resolve())
        result=subprocess.run([str(self.probe),mode],env=env,capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('descriptorsBalanced=true closeErrnoPreserved=true realIoctlForwarded=true gpu=false',result.stdout)
        return result
    def test_idle_process_has_no_bridge_summary(self):
        self.assertEqual(self.run_probe('idle').stderr,'')
    def test_explicit_failed_gpu_open_keeps_diagnostics(self):
        result=self.run_probe('failure')
        self.assertIn('LINUX_BRIDGE openFailed errno=22',result.stderr)
        self.assertIn('LINUX_BRIDGE summary ',result.stderr)
    def test_concurrent_first_gpu_open_and_ordinary_descriptors(self):
        for _ in range(8):
            result=self.run_probe('race')
            self.assertEqual(result.stderr.count('LINUX_BRIDGE openFailed errno=22'),1)
            self.assertEqual(result.stderr.count('LINUX_BRIDGE summary '),1)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--shim',type=Path,required=True)
    args,remaining=parser.parse_known_args();SHIM=args.shim
    unittest.main(argv=[__file__,*remaining],verbosity=2)
