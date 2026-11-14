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


    def test_report_is_one_complete_bounded_record_and_partial_writes_fail(self):
        compiler=shutil.which('cc')
        if compiler is None or os.name!='posix':self.skipTest('POSIX C compiler required')
        import re
        with tempfile.TemporaryDirectory(prefix='omarchy-progress-record-') as temporary:
            folder=Path(temporary);payload=folder/'payload';payload.write_bytes(bytes(range(256))*2)
            source=folder/'report.c'
            source.write_text(r"""
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <stddef.h>
ssize_t instrumented_write(int,const void*,size_t);
#define write instrumented_write
#include "guest_progress_snapshot.h"
#undef write
#include <stdlib.h>
#include <string.h>
static char record[1216];static size_t recorded;static unsigned calls;static const char* mode;
ssize_t instrumented_write(int fd,const void* bytes,size_t size) {
    if(fd!=1 || size>sizeof record) abort();
    ++calls;
    if(!strcmp(mode,"eintr") && calls==1) { errno=EINTR;return -1; }
    recorded=!strcmp(mode,"partial") ? size-1 : size;
    memcpy(record,bytes,recorded);return (ssize_t)recorded;
}
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    mode=argv[2];const int result=wddm_progress_snapshot_report(1,argv[1],getuid());
    const int error=errno;
    if(!strcmp(mode,"partial")) { if(result!=-1 || error!=EIO || calls!=1) return 3; }
    else if(result || calls!=(!strcmp(mode,"eintr") ? 2u : 1u)) return 4;
    printf("REPORT_CONTROL result=%d calls=%u partialRejected=%s gpu=false\n",result,calls,result?"true":"false");
    if(fwrite(record,1,recorded,stdout)!=recorded) return 5;
    return 0;
}
""",encoding='utf-8')
            binary=folder/'report'
            subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(Path(__file__).resolve().parent),str(source),'-o',str(binary)],check=True,capture_output=True,text=True,timeout=30)
            for mode in ('normal','eintr','partial'):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(binary),str(payload),mode],check=True,capture_output=True,text=True,timeout=5)
                    if mode=='partial':self.assertIn('partialRejected=true',result.stdout);continue
                    records=re.findall(r'^NAMED_GRAPHICS_PRODUCER_PROGRESS result=0 errno=0 bytes=512 processIdentityAttested=false gpuTimestamp=false hex=([0-9a-f]+)$',result.stdout,re.M)
                    self.assertEqual(len(records),1);self.assertEqual(bytes.fromhex(records[0]),payload.read_bytes())
            result=subprocess.run([str(binary),str(folder/'missing'),'normal'],check=True,capture_output=True,text=True,timeout=5)
            self.assertRegex(result.stdout,r'PRODUCER_PROGRESS result=-1 errno=[1-9][0-9]* bytes=0 .* hex=\n')


if __name__=='__main__':unittest.main(verbosity=2)
