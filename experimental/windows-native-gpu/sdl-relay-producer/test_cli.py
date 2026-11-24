"""Reject invalid owner/session commands before any GPU API is initialized."""
import argparse
from pathlib import Path
import subprocess


def verify(producer):
    valid='00112233445566778899aabbccddeeff'
    for arguments in ([],[valid],[valid,'0'],[valid,'01'],[valid,'-1'],[valid,'281474976710656'],
                      [valid,'18446744073709551616'],['X'*32,'1'],[valid+'x','1'],[valid,'1x'],
                      [valid,'1','--unknown'],[valid,'0','--bgra']):
        result=subprocess.run([str(producer.resolve()),*arguments],capture_output=True,text=True,timeout=10)
        assert result.returncode==1 and 'namedProducerRejected' in result.stderr, result
        assert 'namedProducerReady' not in result.stdout
    print('PASS: malformed session/generation rejected before GPU initialization')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--producer',type=Path,required=True)
    verify(parser.parse_args().producer)
