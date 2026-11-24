// Private, bounded actual-compositor startup. No Windows display acceptance.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#if defined(WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE) && defined(WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT)
#error Select one compositor output diagnostic
#endif
#if defined(WDDM_BRIDGE_COMPOSITOR_HOST_SCENE) && !defined(WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT)
#error Scene diagnostics require queued presentation
#endif
#if defined(WDDM_BRIDGE_DESKTOP_RESIZE_PROBE) && !defined(WDDM_BRIDGE_COMPOSITOR_HOST_SCENE)
#error Application resize diagnostics require the real desktop scene
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
static _Atomic int scene_ready;
static _Atomic int input_delivery_required,input_response_ready;
static char last_control_reply[8192];
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
#include "hyprland_queue_probe.h"
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
#if HYPRLAND_QUEUE_FRAMES < 200
#error Application resize diagnostics require at least 200 handoffs
#endif
static _Atomic int resize_probe_done;
#endif
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <stddef.h>
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE
#include "hyprland_capture_probe.h"
#endif

typedef int (*main_function)(int,char**,char**);
static main_function compositor_main;
static _Atomic unsigned compositor_commits;
int wddm_bridge_compositor_commit(int fd,uint32_t width,uint32_t height,uint32_t format,uint32_t stride,uint64_t modifier) {
    int (*info)(int,uint64_t*)=(int (*)(int,uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_info");
    uint64_t bytes=0;
    if(!info || !width || !height || width>4096 || height>4096 || stride!=width*4 ||
       info(fd,&bytes) || bytes!=(uint64_t)stride*height) return -1;
    const unsigned frame=atomic_fetch_add(&compositor_commits,1)+1;
    printf("HYPRLAND_COMPOSITOR_COMMIT index=%u width=%u height=%u format=%u stride=%u modifier=%llu ownedNativeBuffer=true pixelsVerified=false windowsPresentation=false\n",
        frame,width,height,format,stride,(unsigned long long)modifier);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE
    if(frame==2 || frame==4) return capture_output(fd,width,height,format,stride);
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
    if(!atomic_load(&scene_ready)) return 0;
    // The input diagnostic must capture the application's response after its
    // real PTY receipt. Keep processing the compositor and input event loops
    // while holding the bounded fixture's remaining GPU handoffs.
    if(atomic_load(&input_delivery_required) && atomic_load(&queued_frames)>=2 && !atomic_load(&input_response_ready)) return 0;
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
    // Ordinary handoffs continue during application buffer churn. Hold only
    // the final audit until the real window has returned to its known layout.
    if(atomic_load(&queued_frames)>=HYPRLAND_QUEUE_FRAMES-1 && !atomic_load(&resize_probe_done)) return 0;
#endif
#endif
    if(frame>=2) return queue_output(fd,width,height,format,stride);
#endif
    return 0;
}
static int control_socket_path(char* target,size_t size) {
    const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime=getenv("XDG_RUNTIME_DIR");
    if(!instance || !instance[0] || !runtime || runtime[0]!='/') return -1;
    const int bytes=snprintf(target,size,"%s/hypr/%s/.socket.sock",runtime,instance);
    return bytes<0 || (size_t)bytes>=size ? -1 : 0;
}
static int run_ctl(char* const arguments[]) {
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
    // Hyprland owns SIGCHLD and may reap a forked hyprctl before waitpid.
    // Use its documented client protocol, without spawning another process.
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(control_socket_path(address.sun_path,sizeof address.sun_path)) return -1;
    char request[1024];
    const int json=arguments[1] && !strcmp(arguments[1],"-j");
    size_t used=(size_t)snprintf(request,sizeof(request),"%s/",json?"j":"");
    for (unsigned index=json?2u:1u;arguments[index];++index) {
        const int count=snprintf(request+used,sizeof(request)-used,"%s%s",used>(json?2u:1u)?" ":"",arguments[index]);
        if (count<0 || (size_t)count>=sizeof(request)-used) return -1;
        used+=(size_t)count;
    }
    const int connection=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (connection<0) return -1;
    // Full desktop startup can still be busy in shader/import initialization.
    // Keep native GPU retirement bounds unchanged; bound only this IPC client.
    const struct timeval timeout={.tv_sec=getenv("WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE") ? 15 : 5};
    int result=-1;const char* stage="connect";size_t received=0;
    struct timespec started={0},finished={0};clock_gettime(CLOCK_MONOTONIC,&started);
    if (setsockopt(connection,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(connection,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) ||
        connect(connection,(struct sockaddr*)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+strlen(address.sun_path)+1))) goto done;
    stage="send";
    for (size_t sent=0;sent<used;) {
        const ssize_t count=send(connection,request+sent,used-sent,MSG_NOSIGNAL);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) goto done;
        sent+=(size_t)count;
    }
    stage="receive";
    char reply[8192];
    for (;;) {
        if (received==sizeof(reply)-1) goto done;
        const ssize_t count=recv(connection,reply+received,sizeof(reply)-1-received,0);
        if (count<0 && errno==EINTR) continue;
        if (count<0) goto done;
        if (!count) break;
        received+=(size_t)count;
    }
    reply[received]='\0';
    stage="reply";
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
    memcpy(last_control_reply,reply,received+1);
    if(strcmp(request,"j/clients"))
#endif
    printf("HYPRLAND_CONTROL_REPLY command=%s reply=%s\n",request,reply);
    clock_gettime(CLOCK_MONOTONIC,&finished);
    printf("HYPRLAND_CONTROL_TIMING timeoutSeconds=%ld elapsedMs=%lld\n",(long)timeout.tv_sec,
        (long long)(finished.tv_sec-started.tv_sec)*1000+(finished.tv_nsec-started.tv_nsec)/1000000);
    const char* first=reply+strspn(reply," \t\r\n");
    result=(received && (json ? (*first=='[' || *first=='{') : !strcmp(reply,"ok"))) ? 0 : -1;
    if(result) errno=EPROTO;
done:
    if(result) {
        const int failure=errno;clock_gettime(CLOCK_MONOTONIC,&finished);
        printf("HYPRLAND_CONTROL_FAILURE command=%s stage=%s errno=%d receivedBytes=%zu elapsedMs=%lld\n",request,stage,failure,received,
            (long long)(finished.tv_sec-started.tv_sec)*1000+(finished.tv_nsec-started.tv_nsec)/1000000);
    }
    if (close(connection)) result=-1;
    return result;
#else
    char signature[512],runtime_env[1024],home_env[1024];
    const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime=getenv("XDG_RUNTIME_DIR");
    const char* home=getenv("HOME");
    if(!instance || snprintf(signature,sizeof(signature),"HYPRLAND_INSTANCE_SIGNATURE=%s",instance)>=(int)sizeof(signature)) return -1;
    if(!runtime || !home || runtime[0]!='/' || home[0]!='/' ||
       snprintf(runtime_env,sizeof runtime_env,"XDG_RUNTIME_DIR=%s",runtime)>=(int)sizeof runtime_env ||
       snprintf(home_env,sizeof home_env,"HOME=%s",home)>=(int)sizeof home_env) return -1;
    char* const environment[]={signature,runtime_env,home_env,
        "LD_LIBRARY_PATH=/opt/wddm-mesa/lib:/usr/lib/x86_64-linux-gnu","PATH=/usr/bin",NULL};
    const pid_t child=fork();
    if(child<0) return -1;
    if(!child) { execve("/usr/bin/hyprctl",arguments,environment); _exit(127); }
    int status=0;
    if(waitpid(child,&status,0)!=child) return -1;
    if(!WIFEXITED(status)) {
        printf("HYPRLAND_STARTUP_CONTROL_SIGNAL number=%d\n",WIFSIGNALED(status)?WTERMSIG(status):0);
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
static int scene_receipt(const char* line) {
    // Keep the message and newline in one syscall. Other guest processes share
    // the serial console; unbuffered puts can split these into separate writes.
    const size_t bytes=strlen(line);char framed[2048];
    if(bytes>=sizeof(framed)-1) { atomic_store(&queue_failed,1);return -1; }
    framed[0]='\n';memcpy(framed+1,line,bytes);
    ssize_t written;
    do { written=write(STDOUT_FILENO,framed,bytes+1); } while(written<0 && errno==EINTR);
    if(written!=(ssize_t)(bytes+1)) { atomic_store(&queue_failed,1);return -1; }
    return 0;
}
static void scene_hex_receipt(const char* label,const char* data,size_t bytes) {
    static const char hex[]="0123456789abcdef";
    char line[17000];
    const int prefix=snprintf(line,sizeof(line),"\n%s ",label);
    if(prefix<=0 || (size_t)prefix+bytes*2+1>=sizeof(line)) return;
    for(size_t index=0;index<bytes;++index) {
        const unsigned char value=(unsigned char)data[index];
        line[prefix+index*2]=hex[value>>4]; line[prefix+index*2+1]=hex[value&15];
    }
    line[prefix+bytes*2]='\n';
    const size_t count=(size_t)prefix+bytes*2+1;
    if(write(STDOUT_FILENO,line,count)!=(ssize_t)count) atomic_store(&queue_failed,1);
}
void wddm_bridge_native_input_log(const char* text,uint32_t bytes) {
    if(text && bytes && bytes<=4096)
        scene_hex_receipt("OMARCHY_NATIVE_INPUT_BACKEND_LOG_HEX",text,bytes);
}
static int native_input_library(struct dl_phdr_info* info,size_t bytes,void* unused) {
    (void)bytes;(void)unused;
    if(info->dlpi_name && strstr(info->dlpi_name,"libaquamarine"))
        scene_hex_receipt("OMARCHY_NATIVE_INPUT_LIBRARY_HEX",info->dlpi_name,strlen(info->dlpi_name));
    return 0;
}
static void scene_diagnostics(void) {
    const char* paths[]={"/tmp/foot-scene.log","/tmp/scene-font-sans","/tmp/scene-font-mono","/tmp/desktop-scene-ready"};
    for(unsigned index=0;index<sizeof(paths)/sizeof(paths[0]);++index) {
        FILE* file=fopen(paths[index],"rb");
        if(!file) { printf("HYPRLAND_SCENE_MISSING file=%s errno=%d\n",paths[index],errno); continue; }
        char data[2048]; const size_t bytes=fread(data,1,sizeof(data),file);
        fclose(file);
        printf("HYPRLAND_SCENE_DIAGNOSTIC file=%s bytes=%zu\n",paths[index],bytes);
        scene_hex_receipt("HYPRLAND_SCENE_DIAGNOSTIC_HEX",data,bytes);
    }
}
static int visible_layer(const char* text,const char* name) {
    const char* match=strstr(text,name);
    if(!match) return 0;
    const char* begin=match;
    while(begin>text && *begin!='{') --begin;
    const char* end=strchr(match,'}');
    if(*begin!='{' || !end) return 0;
    const char* alpha=strstr(begin,"\"alpha\"");
    if(!alpha || alpha>=end) return 0;
    alpha=strchr(alpha,':');if(!alpha || alpha>=end) return 0;
    char* parsed=NULL;errno=0;const double value=strtod(alpha+1,&parsed);
    return !errno && parsed!=alpha+1 && parsed<=end && value>=0.99 && value<=1.0;
}
static int visible_shell_bar(const char* text) { return visible_layer(text,"\"omarchy-bar\""); }
static int visible_gpu_terminal_with_pid(const char* text,pid_t* pid) {
    if(pid) *pid=0;
    const char* match=strstr(text,"\"omarchy-gpu-terminal\"");
    if(!match) return 0;
    // Client records contain nested workspace objects. Track the outer object
    // and JSON strings rather than treating the nearest '{' as the record.
    const char* begin=NULL;const char* end=NULL;
    unsigned depth=0;int quoted=0,escaped=0;
    for(const char* p=text;*p;++p) {
        if(quoted) {
            if(escaped) escaped=0;
            else if(*p=='\\') escaped=1;
            else if(*p=='"') quoted=0;
            continue;
        }
        if(*p=='"') { quoted=1;continue; }
        if(*p=='{') { if(!depth) begin=p;++depth; }
        else if(*p=='}') {
            if(!depth) return 0;
            --depth;
            if(!depth && p>match) { end=p;break; }
        }
    }
    if(!begin || begin>match || !end) return 0;
    const char* mapped=strstr(begin,"\"mapped\"");
    const char* hidden=strstr(begin,"\"hidden\"");
    if(!mapped || mapped>=end || !hidden || hidden>=end) return 0;
    mapped=strchr(mapped,':');hidden=strchr(hidden,':');if(!mapped || !hidden) return 0;
    do { ++mapped; } while(*mapped==' ' || *mapped=='\t');
    do { ++hidden; } while(*hidden==' ' || *hidden=='\t');
    if(strncmp(mapped,"true",4) || strncmp(hidden,"false",5)) return 0;
    if(pid) {
        const char* field=strstr(begin,"\"pid\"");
        if(!field || field>=end || !(field=strchr(field,':')) || field>=end) return 0;
        char* parsed=NULL;errno=0;const long value=strtol(field+1,&parsed,10);
        if(errno || parsed==field+1 || parsed>end || value<=0 || value>INT32_MAX) return 0;
        *pid=(pid_t)value;
    }
    return 1;
}
static int visible_gpu_terminal(const char* text) { return visible_gpu_terminal_with_pid(text,NULL); }
static pid_t last_gpu_terminal_pid;
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
static int resize_gpu_terminal(unsigned phase,unsigned width,unsigned height) {
    char command[256],label[96],receipt[384];
    const unsigned before=atomic_load(&queued_frames),committed_before=atomic_load(&compositor_commits);
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    if(run_ctl(active)) return -1;
    snprintf(label,sizeof label,"OMARCHY_GPU_RESIZE_%u_BEFORE_ACTIVE_HEX",phase);
    scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));
    const int bytes=snprintf(command,sizeof command,
        "hl.dispatch(hl.dsp.window.resize({x=%u,y=%u})); hl.dispatch(hl.dsp.window.center())",width,height);
    if(bytes<=0 || (size_t)bytes>=sizeof command) return -1;
    char* const resize[]={"/usr/bin/hyprctl","eval",command,NULL};
    if(run_ctl(resize)) return -1;
    for(unsigned tick=0;tick<60;++tick) {
        if(run_ctl(active)) return -1;
        const char* size=strstr(last_control_reply,"\"size\"");
        unsigned actual_width=0,actual_height=0;pid_t pid=0;
        const unsigned committed=atomic_load(&compositor_commits);
        if(size && sscanf(size,"\"size\" : [ %u , %u ]",&actual_width,&actual_height)==2 &&
           actual_width==width && actual_height==height &&
           visible_gpu_terminal_with_pid(last_control_reply,&pid) && pid==last_gpu_terminal_pid &&
           committed>=committed_before && committed-committed_before>=3) {
            snprintf(label,sizeof label,"OMARCHY_GPU_RESIZE_%u_AFTER_ACTIVE_HEX",phase);
            scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));
            const int count=snprintf(receipt,sizeof receipt,
                "OMARCHY_GPU_RESIZE phase=%u pid=%d width=%u height=%u beforeQueued=%u afterQueued=%u beforeCommit=%u observedCommit=%u gpuProof=false\n",
                phase,pid,width,height,before,atomic_load(&queued_frames),committed_before,committed);
            return count<=0 || (size_t)count>=sizeof receipt ? -1 : scene_receipt(receipt);
        }
        // Compositor callbacks and ordinary GPU handoffs keep running.
        if(!(tick%5)) {
            char* const repaint[]={"/usr/bin/hyprctl","notify","-1","100","rgb(4477aa)","Resizing GPU application",NULL};
            if(run_ctl(repaint)) return -1;
        }
        usleep(50000);
    }
    return -1;
}
#endif
static int input_delivery_enabled(void) { return !access("/opt/wddm-root/native-input-delivery-probe",R_OK); }
static int input_delivery_snapshot(const char* phase) {
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    char* const cursor[]={"/usr/bin/hyprctl","-j","cursorpos",NULL};
    char label[96];
    if(run_ctl(active)) return -1;
    snprintf(label,sizeof label,"OMARCHY_NATIVE_INPUT_%s_ACTIVE_HEX",phase);
    scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));
    if(run_ctl(cursor)) return -1;
    snprintf(label,sizeof label,"OMARCHY_NATIVE_INPUT_%s_CURSOR_HEX",phase);
    scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));return 0;
}
static int input_delivery_result(void) {
    for(unsigned tick=0;tick<150;++tick) {
        const int file=open("/tmp/omarchy-native-input-received",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(file<0) { if(errno!=ENOENT) return -1;usleep(100000);continue; }
        struct stat metadata;char data[1025];
        if(fstat(file,&metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid!=getuid() || metadata.st_nlink!=1 ||
           metadata.st_size<=0 || metadata.st_size>1024) { close(file);return -1; }
        const ssize_t bytes=read(file,data,sizeof data-1);const int closed=close(file);
        if(bytes<=0 || bytes!=metadata.st_size || closed) return -1;
        data[bytes]=0;scene_hex_receipt("OMARCHY_NATIVE_INPUT_PTY_HEX",data,(size_t)bytes);
        if(!strstr(data,"\"token\":\"omarchyinput93\"")) return -1;
        return 0;
    }
    return -1;
}
static int wait_gpu_retirement(pid_t pid) {
    char path[96];const int count=snprintf(path,sizeof path,"/run/wddm-retired/%d",pid);
    if(pid<=0 || count<=0 || count>=(int)sizeof path) return -1;
    for(unsigned tick=0;tick<120;++tick) {
        struct stat status;errno=0;const int found=lstat(path,&status);
        if(!found && status.st_size>0) {
            if(!S_ISREG(status.st_mode) || status.st_uid || status.st_nlink!=1 || (status.st_mode&0222) || status.st_size>24) return -1;
            FILE* file=fopen(path,"rb");if(!file) return -1;
            char value[32]={0};const size_t bytes=fread(value,1,sizeof(value)-1,file);
            const int bad=ferror(file),closed=fclose(file);if(bad || closed || !bytes) return -1;
            char* end=NULL;errno=0;const unsigned long long actor=strtoull(value,&end,10);
            if(errno || !actor || end==value || strcmp(end,"\n")) return -1;
            char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
            if(run_ctl(clients) || visible_gpu_terminal(last_control_reply)) return -1;
            char line[256];const int length=snprintf(line,sizeof line,
                "OMARCHY_GPU_LIVE_RETIREMENT_OBSERVED pid=%d actor=%llu compositorStillRunning=true rootOwnedWitness=true gpuProof=false\n",pid,actor);
            return length<=0 || length>=(int)sizeof line ? -1 : scene_receipt(line);
        }
        if(found && errno!=ENOENT) return -1;
        // Keep the actual compositor active while its former client's buffers
        // and native actor drain; this is not a VM-shutdown witness.
        if(!(tick%5)) {
            char* const repaint[]={"/usr/bin/hyprctl","notify","-1","100","rgb(4477aa)","Retiring GPU application",NULL};
            if(run_ctl(repaint)) return -1;
        }
        usleep(100000);
    }
    return -1;
}
static int close_gpu_terminal(pid_t pid,int final) {
    char* const close_window[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.window.close({window='class:omarchy-gpu-terminal'}))",NULL};
    if(run_ctl(close_window)) return -1;
    for(unsigned tick=0;tick<50;++tick) {
        char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
        if(!run_ctl(clients) && !strstr(last_control_reply,"\"omarchy-gpu-terminal\"")) {
            if(getenv("WDDM_BRIDGE_DESKTOP_RETIREMENT") && wait_gpu_retirement(pid)) return -1;
            return final ? scene_receipt("OMARCHY_GPU_APPLICATION_CLOSED clientUnmapped=true gpuProof=false\n") : 0;
        }
        usleep(100000);
    }
    return -1;
}
static int prepare_scene(void) {
    atomic_store(&input_delivery_required,input_delivery_enabled());
    if(input_delivery_enabled() && unlink("/tmp/omarchy-native-input-received") && errno!=ENOENT) return -1;
    if(getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE")) {
        // Inspect the shipped desktop without pretending the controlled Foot
        // fixture proves the shell or a normal UWSM session.
        sleep(8);
        char* const layers[]={"/usr/bin/hyprctl","-j","layers",NULL};
        run_ctl(layers);
        if(getenv("WDDM_BRIDGE_ACTOR_PROFILE")) {
            unsigned tick=0;
            for(;tick<40;++tick) {
                if(visible_shell_bar(last_control_reply) && (!getenv("WDDM_BRIDGE_DESKTOP_CONTENT") || visible_layer(last_control_reply,"\"omarchy-background\""))) break;
                // A headless output may otherwise go idle while a windowed
                // EGL client awaits its first compositor frame callback.
                char* const repaint[]={"/usr/bin/hyprctl","notify","-1","250","rgb(4477aa)","Initializing Omarchy GPU desktop",NULL};
                if(run_ctl(repaint)) return -1;
                sleep(1);run_ctl(layers);
            }
            printf("OMARCHY_DESKTOP_COORDINATOR_WAIT shellLayerObserved=%s polls=%u gpuProof=false\n",tick<40 ? "true" : "false",tick);
        }
        scene_hex_receipt("OMARCHY_DESKTOP_LAYERS_HEX",last_control_reply,strlen(last_control_reply));
        if(getenv("WDDM_BRIDGE_DESKTOP_CONTENT")) {
            if(!visible_shell_bar(last_control_reply) || !visible_layer(last_control_reply,"\"omarchy-background\"")) return -1;
            const int live=getenv("WDDM_BRIDGE_DESKTOP_RETIREMENT")!=NULL;
            const unsigned cycles=live ? 5u : 1u;
            for(unsigned cycle=0;cycle<cycles;++cycle) {
                const int final=cycle+1==cycles;
                if(live && unlink("/tmp/omarchy-gpu-terminal-ready") && errno!=ENOENT) return -1;
                char* const launch[]={"/usr/bin/hyprctl","eval",
                    "hl.exec_cmd([[systemd-cat -t omarchy-gpu-app -- /opt/wddm-root/bin/alacritty --config-file /opt/wddm-root/data/alacritty-desktop.toml -vv --class omarchy-gpu-terminal --title 'Omarchy terminal' --hold -e /usr/bin/env -u LD_PRELOAD /bin/bash /opt/wddm-root/data/gpu-terminal-scene.sh]], {float=true, size={1200,800}, center=true})",NULL};
                if(run_ctl(launch)) return -1;
                char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
                unsigned tick=0;
                for(;tick<40;++tick) { if(!run_ctl(clients) && visible_gpu_terminal(last_control_reply)) break;sleep(1); }
                if(final) scene_hex_receipt("OMARCHY_GPU_APPLICATION_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
                printf("OMARCHY_GPU_APPLICATION_WAIT mapped=%s polls=%u gpuProof=false\n",tick<40 ? "true" : "false",tick);
                if(tick==40) return -1;
                char* const place[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.window.resize({x=1200,y=800})); hl.dispatch(hl.dsp.window.center())",NULL};
                if(run_ctl(place)) return -1;
                unsigned content_tick=0;
                for(;content_tick<100 && access("/tmp/omarchy-gpu-terminal-ready",R_OK);++content_tick) usleep(200000);
                if(content_tick==100) return -1;
                if(final) puts("OMARCHY_GPU_APPLICATION_CONTENT_READY ptyMarkerObserved=true gpuProof=false");
                sleep(2);
                if(run_ctl(clients) || !visible_gpu_terminal(last_control_reply)) return -1;
                pid_t pid=0;
                if(live && !visible_gpu_terminal_with_pid(last_control_reply,&pid)) return -1;
                if(final) {
                    last_gpu_terminal_pid=pid;
                    scene_hex_receipt("OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
                } else {
                    scene_hex_receipt("OMARCHY_GPU_LIVE_WARMUP_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
                    char line[160];const int length=snprintf(line,sizeof line,"OMARCHY_GPU_LIVE_WARMUP_READY cycle=%u pid=%d ptyMarkerObserved=true gpuProof=false\n",cycle+1,pid);
                    if(length<=0 || length>=(int)sizeof line || scene_receipt(line) || close_gpu_terminal(pid,0)) return -1;
                }
            }
        }
        if(scene_receipt("OMARCHY_DESKTOP_PROBE_CAPTURE normalConfig=true shellVerified=false applicationGpuVerified=false\n")) return -1;
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
        if(!input_delivery_enabled() || resize_gpu_terminal(1,1440,960)) return -1;
#endif
        if(input_delivery_enabled() && input_delivery_snapshot("BEFORE")) return -1;
        atomic_store(&scene_ready,1);
        return 0;
    }
    char* const launch[]={"/usr/bin/hyprctl","dispatch","exec",
        "[float; size 480 200; move 80 120] LC_ALL=C.UTF-8 XCOMPOSEFILE=/usr/share/X11/locale/en_US.UTF-8/Compose LD_PRELOAD= /usr/bin/foot --config=/foot.ini --app-id=wddm-scene --title='Wayland desktop scene' /bin/sh /desktop-scene.sh > /tmp/foot-scene.log 2>&1",NULL};
    if(run_ctl(launch)) return -1;
    for(unsigned tick=0;tick<100;++tick) {
        char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
        if(!run_ctl(clients) && strstr(last_control_reply,"\"class\": \"wddm-scene\"") && !access("/tmp/desktop-scene-ready",F_OK)) {
            scene_hex_receipt("HYPRLAND_SCENE_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            for(unsigned index=0;index<2;++index) {
                const char* path=index?"/tmp/scene-font-mono":"/tmp/scene-font-sans";
                FILE* file=fopen(path,"rb"); char font[256]={0};
                if(!file) return -1;
                const size_t bytes=fread(font,1,sizeof(font)-1,file);
                const int error=ferror(file),closed=fclose(file);
                if(error || closed || bytes==0 || bytes==sizeof(font)-1 ||
                    strcmp(font,index?"/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf\n":"/usr/share/fonts/noto/NotoSans-Regular.ttf\n")) return -1;
                printf("HYPRLAND_SCENE_FONT index=%u file=%s",index+1,font);
            }
            // Allow the terminal's PTY update to reach its first surface commit,
            // then force actual compositor repaints through the existing loop.
            usleep(200000);
            if(scene_receipt("HYPRLAND_SCENE_READY realWaylandClient=true applicationGpuAccelerated=false compositorGpu=true\n")) return -1;
            atomic_store(&scene_ready,1);
            return 0;
        }
        usleep(100000);
    }
    puts("HYPRLAND_SCENE_FAILURE reason=client-or-font-readiness");
    scene_diagnostics();
    return -1;
}
static int close_scene(void) {
    if(getenv("WDDM_BRIDGE_DESKTOP_CONTENT")) {
        return close_gpu_terminal(last_gpu_terminal_pid,1);
    }
    if(getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE")) return 0;
    char* const close_window[]={"/usr/bin/hyprctl","dispatch","closewindow","class:wddm-scene",NULL};
    if(run_ctl(close_window)) return -1;
    for(unsigned tick=0;tick<50;++tick) {
        char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
        if(!run_ctl(clients) && !strstr(last_control_reply,"\"class\": \"wddm-scene\"") && !access("/tmp/desktop-scene-exit",F_OK)) {
            return scene_receipt("HYPRLAND_SCENE_CLOSED clientUnmapped=true childExitMarker=true\n");
        }
        usleep(100000);
    }
    puts("HYPRLAND_SCENE_FAILURE reason=client-shutdown");
    return -1;
}
#endif
static void* bounded_control(void* unused) {
    (void)unused;
    for(unsigned n=0;n<60;++n) {
        sleep(1);
        char socket[768];
        if(control_socket_path(socket,sizeof socket) || access(socket,F_OK)) continue;
        char* const create[]={"/usr/bin/hyprctl","output","create","headless","WDDM-1",NULL};
        const int created=run_ctl(create);
        printf("HYPRLAND_STARTUP_OUTPUT createExit=%d windowsPresentation=false\n",created);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
        if(getenv("WDDM_BRIDGE_NATIVE_INPUT")) {
            dl_iterate_phdr(native_input_library,NULL);
            char* const devices[]={"/usr/bin/hyprctl","-j","devices",NULL};
            if(run_ctl(devices)) atomic_store(&queue_failed,1);
            else scene_hex_receipt("OMARCHY_NATIVE_INPUT_DEVICES_HEX",last_control_reply,strlen(last_control_reply));
        }
        if(created || prepare_scene()) atomic_store(&queue_failed,1);
        unsigned response_commit=0;int response_observed=0;
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
        unsigned resize_phase=1;
#endif
#endif
        for(unsigned tick=0;tick<HYPRLAND_QUEUE_FRAMES*2+100 && atomic_load(&queued_frames)<HYPRLAND_QUEUE_FRAMES && !atomic_load(&queue_failed);++tick) {
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
            if(atomic_load(&input_delivery_required) && !response_observed && atomic_load(&queued_frames)>=2) {
                if(input_delivery_result()) { atomic_store(&queue_failed,1);break; }
                response_commit=atomic_load(&compositor_commits);response_observed=1;
            }
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
            if(resize_phase==1 && atomic_load(&queued_frames)>=50) {
                if(resize_gpu_terminal(2,1600,1000)) { atomic_store(&queue_failed,1);break; }
                resize_phase=2;
            }
            if(resize_phase==2 && atomic_load(&queued_frames)>=150) {
                if(resize_gpu_terminal(3,1200,800)) { atomic_store(&queue_failed,1);break; }
                resize_phase=3;atomic_store(&resize_probe_done,1);
            }
#endif
#endif
            char* const legacy_repaint[]={"/usr/bin/hyprctl","keyword","decoration:rounding","0",NULL};
            // A Lua config assignment to an unchanged value does not damage a
            // quiet desktop. A short-lived real notification supplies bounded
            // changing content without recreating outputs or hiding warnings.
            char notification[128];
            snprintf(notification,sizeof notification,"GPU composition probe %u; Quickshell GPU integration pending",tick);
            if(getenv("WDDM_BRIDGE_ACTOR_PROFILE")) snprintf(notification,sizeof notification,"4K GPU desktop composition probe %u",tick);
            char* const lua_repaint[]={"/usr/bin/hyprctl","notify","-1","100","rgb(4477aa)",notification,NULL};
            if(getenv("WDDM_BRIDGE_NATIVE_INPUT")) {
                char* const dismiss[]={"/usr/bin/hyprctl","dismissnotify","-1",NULL};
                if(run_ctl(dismiss)) { atomic_store(&queue_failed,1);break; }
            }
            char* const* repaint=getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE") ? lua_repaint : legacy_repaint;
            if (run_ctl(repaint)) { atomic_store(&queue_failed,1); break; }
            usleep(50000);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
            if(response_observed && !atomic_load(&input_response_ready)) {
                const unsigned committed=atomic_load(&compositor_commits);
                if(committed>=response_commit && committed-response_commit>=3) {
                    char receipt[256];const int bytes=snprintf(receipt,sizeof receipt,
                        "OMARCHY_NATIVE_INPUT_RESPONSE_BARRIER receiptCommit=%u observedCommit=%u queuedFrames=%u gpuProof=false\n",
                        response_commit,committed,atomic_load(&queued_frames));
                    if(input_delivery_snapshot("AFTER") || bytes<=0 || (size_t)bytes>=sizeof receipt || scene_receipt(receipt)) {
                        atomic_store(&queue_failed,1);break;
                    }
                    atomic_store(&input_response_ready,1);
                }
            }
#endif
        }
        if (atomic_load(&queued_frames)!=HYPRLAND_QUEUE_FRAMES) atomic_store(&queue_failed,1);
        printf("HYPRLAND_HOST_QUEUE_END frames=%u target=%u failed=%d\n",atomic_load(&queued_frames),HYPRLAND_QUEUE_FRAMES,atomic_load(&queue_failed));
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
        if(atomic_load(&input_delivery_required) && !atomic_load(&input_response_ready)) atomic_store(&queue_failed,1);
#ifdef WDDM_BRIDGE_DESKTOP_RESIZE_PROBE
        if(!atomic_load(&resize_probe_done)) atomic_store(&queue_failed,1);
#endif
        if(close_scene()) atomic_store(&queue_failed,1);
#endif
#else
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE
        const unsigned minimum_commits=4;
#else
        const unsigned minimum_commits=2;
#endif
        for(unsigned tick=0;tick<100 && atomic_load(&compositor_commits)<minimum_commits;++tick) usleep(50000);
#endif
        char* const monitors[]={"/usr/bin/hyprctl","-j","monitors","all",NULL};
        printf("HYPRLAND_STARTUP_MONITORS exit=%d\n",run_ctl(monitors));
        char* const legacy_stop[]={"/usr/bin/hyprctl","dispatch","exit",NULL};
        char* const lua_stop[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.exit())",NULL};
        char* const* stop=getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE") ? lua_stop : legacy_stop;
        puts("HYPRLAND_STARTUP_EXIT_REQUEST method=hyprctl-dispatch-exit");
        run_ctl(stop);
        break;
    }
    sleep(15);
    puts("HYPRLAND_STARTUP_STOP reason=bounded-diagnostic signal=TERM windowsPresentation=false");
    kill(getpid(),SIGTERM);
    return NULL;
}
static int startup_main(int argc,char** argv,char** environment) {
    if(argc<1 || strcmp(argv[0],"/guest-probe")) return compositor_main(argc,argv,environment);
    setvbuf(stdout,NULL,_IONBF,0);
    const char* runtime=getenv("WDDM_BRIDGE_HYPRLAND_RUNTIME");
    const char* home=getenv("WDDM_BRIDGE_HYPRLAND_HOME");
    if((runtime && !home) || (home && !runtime) || (runtime && (runtime[0]!='/' || home[0]!='/'))) return 121;
    if(!runtime) { runtime="/tmp/hypr-runtime";home="/tmp/hypr-home"; }
    if(setenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE","true",1) ||
       setenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS","true",1) ||
       setenv("AQ_EXPERIMENTAL_NATIVE_RENDER","1",1) ||
       setenv("GBM_BACKENDS_PATH","/opt/wddm-mesa/lib/gbm",1) ||
       setenv("XDG_RUNTIME_DIR",runtime,1) ||
       setenv("HOME",home,1) ||
       setenv("HYPRLAND_LOG_WLR","1",1) ||
       setenv("HYPRLAND_TRACE","1",1)) return 121;
    if((mkdir(runtime,0700) && errno!=EEXIST) ||
       (mkdir(home,0700) && errno!=EEXIST)) return 122;
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
    const unsigned profile=getenv("WDDM_BRIDGE_ACTOR_PROFILE") && !strcmp(getenv("WDDM_BRIDGE_ACTOR_PROFILE"),"13") ? 13 : getenv("WDDM_BRIDGE_ACTOR_PROFILE") && !strcmp(getenv("WDDM_BRIDGE_ACTOR_PROFILE"),"12") ? 12 : getenv("WDDM_BRIDGE_ACTOR_PROFILE") && !strcmp(getenv("WDDM_BRIDGE_ACTOR_PROFILE"),"11") ? 11 : getenv("WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE") ? 10 : getenv("WDDM_BRIDGE_DESKTOP_LARGE_MAP_PROBE") ? 9 : getenv("WDDM_BRIDGE_DESKTOP_SPARSE_PROBE") ? 8 : getenv("WDDM_BRIDGE_DESKTOP_4K_CPU_PROBE") ? 7 : getenv("WDDM_BRIDGE_DESKTOP_4K_PROBE") ? 6 : 5;
    const char* begin_name=profile==13 ? "wddm_bridge_begin_opengl_desktop_reference_render" : profile==12 ? "wddm_bridge_begin_opengl_desktop_slot_render" : profile==11 ? "wddm_bridge_begin_opengl_desktop_fence_render" : profile==10 ? "wddm_bridge_begin_opengl_desktop_presentation_render" : profile==9 ? "wddm_bridge_begin_opengl_desktop_large_map_render" : profile==8 ? "wddm_bridge_begin_opengl_desktop_sparse_render" : profile==7 ? "wddm_bridge_begin_opengl_desktop_4k_cpu_render" : profile==6 ? "wddm_bridge_begin_opengl_desktop_4k_render" : "wddm_bridge_begin_opengl_desktop_render";
#else
    const unsigned profile=4;
    const char* begin_name="wddm_bridge_begin_opengl_compositor_render";
#endif
    int (*begin)(void)=(int (*)(void))dlsym(RTLD_DEFAULT,begin_name);
    if(!begin || begin()) { perror("Hyprland profile negotiation"); return 123; }
    printf("HYPRLAND_STARTUP_BEGIN version=0.56.2 boundedProfile=%u actualCompositor=true windowsPresentation=false\n",profile);
    pthread_t thread;
    if(pthread_create(&thread,NULL,bounded_control,NULL) || pthread_detach(thread)) return 124;
    const char* config=getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE") ?
        "/opt/wddm-root/data/normal-desktop.lua" : "/hyprland.conf";
    char* arguments[]={"/guest-probe","--config",(char*)config,NULL};
    const int result=compositor_main(3,arguments,environment);
    printf("HYPRLAND_STARTUP_MAIN_RETURN exit=%d windowsPresentation=false\n",result);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
    if (atomic_load(&queue_failed) || atomic_load(&queued_frames)!=HYPRLAND_QUEUE_FRAMES) return 125;
#endif
    return result;
}
int __libc_start_main(main_function main,int argc,char** argv,void (*init)(void),
    void (*fini)(void),void (*rtld_fini)(void),void* stack_end) {
    typedef int (*start_function)(main_function,int,char**,void (*)(void),void (*)(void),void (*)(void),void*);
    start_function original=(start_function)dlsym(RTLD_NEXT,"__libc_start_main");
    if(!original) _exit(120);
    compositor_main=main;
    return original(startup_main,argc,argv,init,fini,rtld_fini,stack_end);
}
