"""Actual producer C controls using virtual POSIX clocks/mock IO; no GPU or VM."""
from pathlib import Path
import importlib.util,json,os,shutil,subprocess,tempfile,unittest

WRAPPER=r"""
#define main pulse_main
#define clock_gettime virtual_clock_gettime
#define clock_nanosleep virtual_clock_nanosleep
#define open virtual_open
#define close virtual_close
#define write virtual_write
#define rename virtual_rename
#define unlink virtual_unlink
#include "pulse.c"
#undef main
#undef clock_gettime
#undef clock_nanosleep
#undef open
#undef close
#undef write
#undef rename
#undef unlink
#include <stdlib.h>
#include <string.h>
static const char* scenario;
static uint64_t current_ns=1000000000ull;
static unsigned sleeps_count,terminal_writes,interrupted;
static uint64_t final_write_ns;
static char captured_receipt[512];
int virtual_clock_gettime(clockid_t id,struct timespec* value) {
    if(!strcmp(scenario,"clock-error")) return -1;
    if(id!=CLOCK_MONOTONIC) return -1;
    value->tv_sec=(time_t)(current_ns/1000000000ull);
    value->tv_nsec=(long)(current_ns%1000000000ull);return 0;
}
int virtual_clock_nanosleep(clockid_t id,int flags,const struct timespec* target,struct timespec* remainder) {
    (void)remainder;
    if(id!=CLOCK_MONOTONIC || flags!=TIMER_ABSTIME) return EINVAL;
    ++sleeps_count;
    if(!strcmp(scenario,"sleep-error")) return EIO;
    if(!strcmp(scenario,"sleep-eintr") && !interrupted++) return EINTR;
    const uint64_t next=(uint64_t)target->tv_sec*1000000000ull+(uint64_t)target->tv_nsec;
    if(next>current_ns) current_ns=next;
    if(!strcmp(scenario,"late") && sleeps_count%10==0) current_ns+=100000001ull;
    if(current_ns>=4000000000ull) {
        int signal_number=0;
        if(!strcmp(scenario,"term")) signal_number=SIGTERM;
        if(!strcmp(scenario,"hup")) signal_number=SIGHUP;
        if(!strcmp(scenario,"int")) signal_number=SIGINT;
        if(signal_number) { if(raise(signal_number)) abort();return EINTR; }
    }
    return 0;
}
int virtual_open(const char* name,int flags,...) {
    if(strncmp(name,"/tmp/omarchy-active-terminal-",29) ||
       (flags&(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW))!=(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW)) abort();
    return 123;
}
int virtual_close(int fd) { if(fd!=123) abort();return 0; }
int virtual_unlink(const char* path) { (void)path;return 0; }
int virtual_rename(const char* from,const char* to) {
    (void)from;if(strcmp(to,"/tmp/omarchy-active-terminal-pulse.json")) abort();return 0;
}
ssize_t virtual_write(int fd,const void* data,size_t count) {
    if(fd==STDOUT_FILENO) {
        if(!strcmp(scenario,"write-error")) { errno=EPIPE;return -1; }
        if(!strcmp(scenario,"write-eintr") && !interrupted++) { errno=EINTR;return -1; }
        ++terminal_writes;final_write_ns=current_ns;
    } else {
        if(fd!=123 || count>=sizeof captured_receipt) abort();
        memcpy(captured_receipt,data,count);captured_receipt[count]=0;
    }
    return (ssize_t)count;
}
int main(int argc,char** argv) {
    if(argc!=2) return 1;
    scenario=argv[1];const int code=pulse_main();
    printf("{\"returnCode\":%d,\"terminalWrites\":%u,\"sleeps\":%u,\"lastWriteNs\":%" PRIu64 ",\"receipt\":%s,\"gpu\":false,\"clock\":\"virtual\",\"io\":\"mock\"}\n",
        code,terminal_writes,sleeps_count,final_write_ns,captured_receipt[0]?captured_receipt:"{}");
    return 0;
}
"""


class SustainedProducer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('cc')
        if compiler is None or os.name!='posix':raise unittest.SkipTest('POSIX C compiler required')
        gpu=Path(__file__).resolve().parent
        spec=importlib.util.spec_from_file_location('sustained_pulse',gpu/'active-frame-probe/generate_sustained_pulse.py')
        cls.generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.generator)
        cls.original=(gpu/'active-frame-probe/terminal_pulse.c').read_bytes();cls.derived=cls.generator.derive(cls.original)
        cls.temporary=tempfile.TemporaryDirectory(prefix='omarchy-pulse-clock-');cls.addClassCleanup(cls.temporary.cleanup);cls.binaries={}
        for name,data in (('original',cls.original),('sustained',cls.derived)):
            folder=Path(cls.temporary.name)/name;folder.mkdir();(folder/'pulse.c').write_bytes(data)
            (folder/'control.c').write_text(WRAPPER,encoding='utf-8');binary=folder/'control'
            result=subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(folder/'control.c'),'-o',str(binary)],capture_output=True,text=True,timeout=30)
            if result.returncode:raise RuntimeError(result.stderr)
            cls.binaries[name]=binary
    def run_case(self,scenario,profile='sustained'):
        result=subprocess.run([str(self.binaries[profile]),scenario],capture_output=True,text=True,check=True,timeout=5)
        value=json.loads(result.stdout);self.assertFalse(value['gpu']);self.assertEqual(value['clock'],'virtual');self.assertEqual(value['io'],'mock');return value
    def test_original_fixture_stops_before_ninety_seconds(self):
        value=self.run_case('normal','original');self.assertEqual(value['returnCode'],0)
        self.assertEqual(value['terminalWrites'],3600);self.assertLess(value['receipt']['lastNs']-value['receipt']['startedNs'],90_000_000_000)
    def test_sustained_fixture_covers_ninety_with_bounded_updates(self):
        value=self.run_case('normal');self.assertEqual(value['returnCode'],0);self.assertEqual(value['terminalWrites'],7200)
        receipt=value['receipt'];self.assertEqual(receipt['updates'],7200)
        self.assertGreater(receipt['lastNs']-receipt['startedNs'],90_000_000_000)
        self.assertLess(receipt['lastNs']-receipt['startedNs'],120_000_000_000)
    def test_late_producer_skips_slots_and_stops_scheduled_writes(self):
        value=self.run_case('late');self.assertEqual(value['returnCode'],0)
        self.assertGreater(value['receipt']['skippedSlots'],0);self.assertGreater(value['receipt']['maximumLateNs'],0)
        self.assertLess(value['terminalWrites'],7200);self.assertGreater(value['receipt']['lastNs']-value['receipt']['startedNs'],90_000_000_000)
        self.assertLess(value['lastWriteNs']-value['receipt']['startedNs'],120_000_000_000)
    def test_original_signal_handlers_stop_the_owned_producer(self):
        for scenario in ('term','hup','int'):
            with self.subTest(scenario=scenario):
                value=self.run_case(scenario);self.assertEqual(value['returnCode'],0)
                self.assertGreater(value['terminalWrites'],0);self.assertLess(value['terminalWrites'],200)
    def test_interrupted_absolute_sleep_and_write_preserve_updates(self):
        for scenario in ('sleep-eintr','write-eintr'):
            with self.subTest(scenario=scenario):
                value=self.run_case(scenario);self.assertEqual(value['returnCode'],0);self.assertEqual(value['terminalWrites'],7200)
    def test_clock_sleep_and_output_failures_preserve_legacy_outcomes(self):
        for scenario,code in (('clock-error',2),('sleep-error',3),('write-error',0)):
            with self.subTest(scenario=scenario):
                value=self.run_case(scenario);self.assertEqual(value['returnCode'],code);self.assertEqual(value['terminalWrites'],0)
    def test_changed_parent_source_is_rejected(self):
        with self.assertRaises(ValueError):self.generator.derive(self.original+b'\n')


if __name__=='__main__':unittest.main(verbosity=2)
