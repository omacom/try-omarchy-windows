#define _GNU_SOURCE
#include "trace_session.h"

static void require(int value) { if(!value) { fputs("TRACE_CONTROL_FAILED\n",stderr);exit(1); } }
int main(void) {
    (void)&trace_session_begin;
    dev_t device=0;
    require(!trace_identity("org.7wdev.graphics.trace\n","242:2\n",&device) && device==makedev(242,2));
    const char* invalid[]={"0:2\n","4096:2\n","242:1048576\n","0242:2\n","242:02\n","242:2x","242:2\nextra","-1:2\n","42949672960:2\n"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];++i) require(trace_identity("org.7wdev.graphics.trace\n",invalid[i],&device)!=0);
    require(trace_identity("org.7wdev.wddm\n","242:2\n",&device)!=0);
    FILE* input=tmpfile();FILE* output=tmpfile();require(input && output);
    const char payload[]="complete GPU receipts\nincluding first failure\n";
    require(fwrite(payload,1,sizeof payload-1,input)==sizeof payload-1 && !fflush(input));rewind(input);
    require(!trace_pump_fd(fileno(input),fileno(output),1024));
    require(!fflush(output));rewind(output);
    char actual[512]={0};const size_t bytes=fread(actual,1,sizeof actual-1,output);
    require(bytes>sizeof payload && strstr(actual,"WDDM_VIRTIO_TRACE_BEGIN version=1 maxBytes=1024\n") &&
            strstr(actual,payload) && strstr(actual,"WDDM_VIRTIO_TRACE_END bytes=46 fnv1a=daca8cc128dcd49a complete=true\n"));
    FILE* rejected=tmpfile();require(rejected!=NULL);rewind(input);
    require(trace_pump_fd(fileno(input),fileno(rejected),8)==125);
    require(!fflush(rejected));rewind(rejected);memset(actual,0,sizeof actual);
    require(fread(actual,1,sizeof actual-1,rejected)>0 && !strstr(actual,"complete=true"));
    require(!fclose(input) && !fclose(output) && !fclose(rejected));
    puts("VIRTIO_TRACE_CONTROL namedPortOnly=true canonicalIdentity=true orderedBytes=true boundedPump=true overflowRejected=true liveFraming=true gpu=false");
    return 0;
}
