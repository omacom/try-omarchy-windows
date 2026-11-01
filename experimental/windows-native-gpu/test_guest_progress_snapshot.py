"""CPU-only controls for bounded diagnostic file reads; no VM or GPU use."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest


class ProgressSnapshot(unittest.TestCase):
    def test_real_file_types_ownership_size_and_descriptor_cleanup(self):
        compiler=shutil.which('cc')
        if compiler is None or os.name!='posix':
            self.skipTest('POSIX C compiler required')
        with tempfile.TemporaryDirectory(prefix='omarchy-progress-') as temporary:
            directory=Path(temporary)
            (directory/'small').write_bytes(b'{"updates":123,"gpuTimestamp":false}\n')
            (directory/'maximum').write_bytes(b'x'*512)
            (directory/'large').write_bytes(b'x'*513)
            (directory/'empty').touch()
            (directory/'link').symlink_to(directory/'small')
            os.mkfifo(directory/'fifo')
            source=directory/'control.c'
            source.write_text(r'''
#define _POSIX_C_SOURCE 200809L
#include "guest_progress_snapshot.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned descriptors(void) {
    DIR* directory=opendir("/proc/self/fd");if(!directory) abort();
    unsigned count=0;while(readdir(directory)) ++count;
    if(closedir(directory)) abort();
    return count;
}
int main(int argc,char** argv) {
    if(argc!=2 || chdir(argv[1])) return 1;
    unsigned char output[WDDM_PROGRESS_SNAPSHOT_MAX];size_t bytes=0;
    const unsigned before=descriptors();
    const char expected[]="{\"updates\":123,\"gpuTimestamp\":false}\n";
    for(unsigned repeat=0;repeat<200;++repeat) {
        if(wddm_progress_snapshot("small",getuid(),output,&bytes) || bytes!=sizeof(expected)-1 ||
           memcmp(output,expected,bytes)) return 2;
        if(wddm_progress_snapshot("maximum",getuid(),output,&bytes) || bytes!=512) return 3;
        const char* paths[]={"large","empty","link","fifo",".","missing"};
        for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
            bytes=777;errno=0;
            if(!wddm_progress_snapshot(paths[i],getuid(),output,&bytes) || bytes || !errno) return 4;
        }
        bytes=777;errno=0;
        if(!wddm_progress_snapshot("small",getuid()+1,output,&bytes) || bytes || errno!=EPERM) return 5;
    }
    if(descriptors()!=before) return 6;
    puts("PROGRESS_SNAPSHOT_CONTROL regular=true sizeCap=512 symlinkRejected=true fifoNonblocking=true wrongOwnerRejected=true descriptorLeak=false gpu=false");
    return 0;
}
''',encoding='utf-8')
            binary=directory/'control'
            subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-I'+str(Path(__file__).resolve().parent),str(source),'-o',str(binary)],
                check=True,capture_output=True,text=True,timeout=30)
            result=subprocess.run([str(binary),str(directory)],check=True,capture_output=True,text=True,timeout=10)
            self.assertIn('sizeCap=512',result.stdout)
            self.assertIn('descriptorLeak=false gpu=false',result.stdout)


if __name__=='__main__':unittest.main(verbosity=2)
