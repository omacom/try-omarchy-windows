"""Root broker session selection must preserve the diagnostic default exactly."""
import hashlib,importlib.util,tempfile,unittest
from pathlib import Path
GPU=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('launcher_guest_broker_session',GPU/'launcher-guest-bootstrap/generate.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class RootBrokerSession(unittest.TestCase):
    def test_default_is_byte_identical_and_session_is_one_root_environment_insertion(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-broker-session-') as temporary:
            root=Path(temporary);original=module.generate(GPU,root/'default');session=module.generate(GPU,root/'session',broker_session=True)
            baseline=(root/'default/normal_root_graphics_service.c').read_bytes()
            self.assertEqual(hashlib.sha256(baseline).hexdigest(),'c867e66968c0d586246e5978ca9c0b3138d6c65a8bf91a0dfddd573ab28e562d')
            selected=(root/'session/normal_root_graphics_service.c').read_text()
            addition=('    if(setenv("WDDM_BRIDGE_BROKER_SESSION","1",1)) { perror("broker session environment");return 125; }\n'
                      '    puts("NAMED_GRAPHICS_BROKER_SESSION requested=true rootSupervisorOwned=true normalOmarchySession=false");\n')
            self.assertEqual(selected.count(addition),1);self.assertEqual(selected.replace(addition,'',1).encode(),baseline)
            self.assertFalse(original['guestBrokerSessionEnabled']);self.assertTrue(session['guestBrokerSessionEnabled'])
            self.assertEqual(session['guestBrokerLifetimeOwner'],'pinned-root-supervisor')
            for value in (original,session):
                self.assertTrue(value['probeSessionStillBounded']);self.assertFalse(value['guestRootSupervisorLifetimeChanged'])
                self.assertFalse(value['normalAccountBootstrapImplemented']);self.assertFalse(value['inputImplementationChanged']);self.assertFalse(value['dailyUseAccepted'])
    def test_truthy_non_booleans_do_not_select_a_privileged_mode(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-broker-option-') as temporary:
            for value in (0,1,None,'1',[]):
                with self.subTest(value=value),self.assertRaises(ValueError):module.generate(GPU,Path(temporary)/'unused',broker_session=value)
if __name__=='__main__':unittest.main(verbosity=2)
