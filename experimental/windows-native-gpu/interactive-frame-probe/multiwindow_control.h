/* Owned diagnostic window control through existing Hyprland IPC. */
#pragma once
static pid_t multiwindow_companion_pid,browser_gpu_pid;
static unsigned int browser_launch_count;
static int browser_receipt(void) {
    const int fd=open("/tmp/wddm-browser-receipt.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return -1;
    struct stat status;char body[4097];
    if(fstat(fd,&status) || !S_ISREG(status.st_mode) || status.st_uid!=getuid() || status.st_nlink!=1 ||
       status.st_size<=0 || status.st_size>4096) { close(fd);return -1; }
    const ssize_t bytes=read(fd,body,sizeof body-1);const int closed=close(fd);
    if(bytes!=status.st_size || closed || bytes<=0) return -1;
    body[bytes]=0;
    const char* field=strstr(body,"\"gpuPid\"");char* end=NULL;long value=0;
    if(!field || !(field=strchr(field,':'))) return -1;
    errno=0;value=strtol(field+1,&end,10);
    if(errno || end==field+1 || value<=0 || value>INT32_MAX) return -1;
    browser_gpu_pid=(pid_t)value;
    scene_hex_receipt("OMARCHY_BROWSER_CDP_RECEIPT_HEX",body,(size_t)bytes);return 0;
}
static int multiwindow_focus(int browser,unsigned phase) {
    char* const focus[]={"/usr/bin/hyprctl","eval",browser?
        "hl.dispatch(hl.dsp.focus({window='class:omarchy-gpu-browser'}))":
        "hl.dispatch(hl.dsp.focus({window='class:omarchy-gpu-terminal'}))",NULL};
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    if(run_ctl(focus) || run_ctl(active)) return -1;
    char label[96];const int count=snprintf(label,sizeof label,"OMARCHY_BROWSER_%u_%s_ACTIVE_HEX",phase,browser?"BROWSER":"TERMINAL");
    if(count<=0 || (size_t)count>=sizeof label) return -1;
    scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));return 0;
}
static int browser_completed_failure(void) {
    const int fd=open("/tmp/wddm-browser-failure.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return errno==ENOENT ? 0 : -1;
    struct stat status;char body[1025];
    if(fstat(fd,&status) || !S_ISREG(status.st_mode) || status.st_uid!=getuid() || status.st_nlink!=1 ||
       (status.st_mode&0222) || status.st_size<=0 || status.st_size>1024) { close(fd);return -1; }
    const ssize_t bytes=read(fd,body,sizeof body-1);const int closed=close(fd);
    if(bytes!=status.st_size || closed || bytes<=0) return -1;
    body[bytes]=0;
    scene_hex_receipt("OMARCHY_BROWSER_COMPLETED_FAILURE_HEX",body,(size_t)bytes);
    const char* field=strstr(body,"\"browserPid\"");char* end=NULL;long browser=0;
    if(field && (field=strchr(field,':'))) { errno=0;browser=strtol(field+1,&end,10);if(errno || end==field+1 || browser<=0 || browser>INT32_MAX) browser=0; }
    if(browser>0) {
        char path[160],diagnostic[1901];
        const int length=snprintf(path,sizeof path,"/tmp/wddm-browser-final-diagnostics-%ld.json",browser);
        const int diagnostic_fd=length>0 && (size_t)length<sizeof path ? open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK) : -1;
        if(diagnostic_fd>=0) {
            struct stat info;
            const int valid=!fstat(diagnostic_fd,&info) && S_ISREG(info.st_mode) && info.st_uid==getuid() &&
                info.st_nlink==1 && !(info.st_mode&0222) && info.st_size>0 && info.st_size<=1900;
            const ssize_t count=valid ? read(diagnostic_fd,diagnostic,sizeof diagnostic) : -1;
            const int closed=close(diagnostic_fd);
            if(count>0 && count==info.st_size && !closed) scene_hex_receipt("OMARCHY_BROWSER_FINAL_DIAGNOSTICS_HEX",diagnostic,(size_t)count);
        }
        const int focus_length=snprintf(path,sizeof path,"/tmp/wddm-browser-sdl-focus-%ld.json",browser);
        const int focus_fd=focus_length>0 && (size_t)focus_length<sizeof path ? open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK) : -1;
        if(focus_fd>=0) {
            struct stat info;
            const int valid=!fstat(focus_fd,&info) && S_ISREG(info.st_mode) && info.st_uid==getuid() &&
                info.st_nlink==1 && !(info.st_mode&0222) && info.st_size>0 && info.st_size<=1900;
            const ssize_t count=valid ? read(focus_fd,diagnostic,sizeof diagnostic) : -1;
            const int closed=close(focus_fd);
            if(count>0 && count==info.st_size && !closed) scene_hex_receipt("OMARCHY_BROWSER_FINAL_FOCUS_HEX",diagnostic,(size_t)count);
        }
    }
    puts("OMARCHY_BROWSER_FAILURE stage=completed-browser-probe accepted=false");
    return 1;
}
static int arm_browser_input_reader(void) {
    pid_t focused=0;
    if(!visible_gpu_terminal_with_pid(last_control_reply,&focused) || focused!=last_gpu_terminal_pid) return -1;
    const char* temporary="/tmp/wddm-browser-input-armed.tmp";
    const int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    char body[96];const int bytes=snprintf(body,sizeof body,"%d:%d\n",multiwindow_companion_pid,browser_gpu_pid);
    const int valid=bytes>0 && (size_t)bytes<sizeof body && write(fd,body,(size_t)bytes)==bytes && !fchmod(fd,0400);
    const int closed=close(fd);
    if(!valid || closed || rename(temporary,"/tmp/wddm-browser-input-armed")) { unlink(temporary);return -1; }
    printf("OMARCHY_BROWSER_INPUT_ARMED terminalPid=%d browserPid=%d gpuPid=%d actualPtyReadTimeoutSeconds=30 inputImplementationChanged=false\n",
        last_gpu_terminal_pid,multiwindow_companion_pid,browser_gpu_pid);
    return 0;
}
static int multiwindow_start(void) {
    if(unlink("/tmp/wddm-browser-input-armed") && errno!=ENOENT) return -1;
    if(unlink("/tmp/wddm-browser-failure.json") && errno!=ENOENT) return -1;
    if(unlink("/tmp/wddm-browser-receipt.json") && errno!=ENOENT) return -1;
    char* const launch[]={"/usr/bin/hyprctl","eval",
        browser_launch_count++ ?
        "hl.exec_cmd([[systemd-cat -t omarchy-gpu-companion -- /usr/bin/python3 /opt/wddm-root/bin/chromium-gpu-probe.py]], {float=true, size={1400,1000}})" :
        "hl.exec_cmd([[systemd-cat -t omarchy-gpu-companion -- /usr/bin/python3 /opt/wddm-root/bin/chromium-gpu-probe.py --sdl-input]], {float=true, size={1400,1000}})",NULL};
    if(run_ctl(launch)) return -1;
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    const uint64_t startup_deadline=active_monotonic_ns()+75000000000ull;
    puts("OMARCHY_BROWSER_STARTUP_DIAGNOSTIC originalDeadlineSeconds=30 diagnosticDeadlineSeconds=75 predicatesChanged=false performanceAccepted=false");
    for(unsigned tick=0;tick<750 && active_monotonic_ns()<startup_deadline;++tick) {
        if(browser_completed_failure()) return -1;
        if(run_ctl(active)) return -1;
        if(tick%50==0) {
            printf("OMARCHY_BROWSER_STARTUP_OBSERVATION tick=%u monotonicNs=%llu compositorCommits=%u queuedFrames=%u receiptPresent=%s accepted=false\n",
                tick,(unsigned long long)active_monotonic_ns(),atomic_load(&compositor_commits),atomic_load(&queued_frames),
                access("/tmp/wddm-browser-receipt.json",R_OK) ? "false" : "true");
            scene_hex_receipt("OMARCHY_BROWSER_STARTUP_ACTIVE_HEX",last_control_reply,strlen(last_control_reply));
        }
        if(strstr(last_control_reply,"\"omarchy-gpu-browser\"") && !access("/tmp/wddm-browser-receipt.json",R_OK)) {
            const char* field=strstr(last_control_reply,"\"pid\"");char* end=NULL;long value=0;
            if(!field || !(field=strchr(field,':'))) return -1;
            errno=0;value=strtol(field+1,&end,10);
            if(errno || end==field+1 || value<=0 || value>INT32_MAX) return -1;
            multiwindow_companion_pid=(pid_t)value;
            char* const place[]={"/usr/bin/hyprctl","eval",
                "hl.dispatch(hl.dsp.window.float({action='enable',window='class:omarchy-gpu-browser'})); hl.dispatch(hl.dsp.window.resize({x=1400,y=1000,window='class:omarchy-gpu-browser'})); hl.dispatch(hl.dsp.window.move({x=2200,y=180,relative=false,window='class:omarchy-gpu-browser'}))",NULL};
            if(browser_receipt() || run_ctl(place) || multiwindow_focus(1,0)) return -1;
            int layout_ready=0;
            for(unsigned settle=0;settle<30;++settle) {
                if(run_ctl(active)) return -1;
                const char* size=strstr(last_control_reply,"\"size\"");
                const char* at=strstr(last_control_reply,"\"at\"");
                unsigned width=0,height=0;int x=0,y=0;
                if(size && at && sscanf(size,"\"size\" : [ %u , %u ]",&width,&height)==2 &&
                   sscanf(at,"\"at\" : [ %d , %d ]",&x,&y)==2 && width==1400 && height==1000 && x==2200 && y==180 &&
                   strstr(last_control_reply,"\"floating\": true") && strstr(last_control_reply,"\"class\": \"omarchy-gpu-browser\"")) {
                    layout_ready=1;break;
                }
                usleep(100000);
            }
            if(!layout_ready) { puts("OMARCHY_BROWSER_FAILURE stage=floating-layout");return -1; }
            scene_hex_receipt("OMARCHY_BROWSER_LAYOUT_ACTIVE_HEX",last_control_reply,strlen(last_control_reply));
            printf("OMARCHY_BROWSER_LAYOUT_READY browserPid=%d width=1400 height=1000 x=2200 y=180 floating=true officialInputChanged=false\n",multiwindow_companion_pid);
            if(run_ctl(clients)) return -1;
            scene_hex_receipt("OMARCHY_BROWSER_READY_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            if(multiwindow_focus(0,0) || arm_browser_input_reader()) return -1;
            printf("OMARCHY_BROWSER_WINDOW_READY terminalPid=%d browserPid=%d gpuPid=%d queuedFrames=%u gpuProof=false\n",
                last_gpu_terminal_pid,multiwindow_companion_pid,browser_gpu_pid,atomic_load(&queued_frames));
            return 0;
        }
        usleep(100000);
    }
    puts("OMARCHY_BROWSER_FAILURE stage=window-or-sandboxed-gpu-start");return -1;
}
static int browser_interaction_checkpoint(unsigned phase) {
    char request[128],done[128],body[96],reply[96];
    const int a=snprintf(request,sizeof request,"/tmp/wddm-browser-interaction-%d",multiwindow_companion_pid);
    const int b=snprintf(done,sizeof done,"/tmp/wddm-browser-interaction-done-%d",multiwindow_companion_pid);
    const int bytes=snprintf(body,sizeof body,"%d:%d\n",multiwindow_companion_pid,browser_gpu_pid);
    if(a<=0 || (size_t)a>=sizeof request || b<=0 || (size_t)b>=sizeof done || bytes<=0 || (size_t)bytes>=sizeof body) return -1;
    char temporary[160];const int n=snprintf(temporary,sizeof temporary,"%s.tmp",request);
    if(n<=0 || (size_t)n>=sizeof temporary) return -1;
    const int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    const int valid=write(fd,body,(size_t)bytes)==bytes && !fchmod(fd,0400);
    const int closed=close(fd);if(!valid || closed || rename(temporary,request)) return -1;
    for(unsigned tick=0;tick<(phase==1 ? 850u : 200u);++tick) {
        if(browser_completed_failure()) return -1;
        const int answer=open(done,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(answer>=0) {
            struct stat status;
            const int valid_reply=!fstat(answer,&status) && S_ISREG(status.st_mode) && status.st_uid==getuid() &&
                status.st_nlink==1 && !(status.st_mode&0222) && status.st_size==bytes;
            const ssize_t count=valid_reply ? read(answer,reply,sizeof reply) : -1;
            const int close_result=close(answer);
            if(count!=bytes || close_result || memcmp(reply,body,(size_t)bytes)) return -1;
            char proof_path[160],proof_body[1901];struct stat proof_status;
            const int n=snprintf(proof_path,sizeof proof_path,"/tmp/wddm-browser-interaction-proof-%d.json",multiwindow_companion_pid);
            if(n<=0 || (size_t)n>=sizeof proof_path) return -1;
            const int proof=open(proof_path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
            if(proof<0) return -1;
            const int valid_proof=!fstat(proof,&proof_status) && S_ISREG(proof_status.st_mode) &&
                proof_status.st_uid==getuid() && proof_status.st_nlink==1 && !(proof_status.st_mode&0222) &&
                proof_status.st_size>0 && proof_status.st_size<=1900;
            const ssize_t received=valid_proof ? read(proof,proof_body,sizeof proof_body) : -1;
            const int proof_closed=close(proof);
            if(received<=0 || received!=proof_status.st_size || proof_closed) return -1;
            scene_hex_receipt("OMARCHY_BROWSER_INTERACTION_PROOF_HEX",proof_body,(size_t)received);
            printf("OMARCHY_BROWSER_INTERACTION_ACK browserPid=%d gpuPid=%d ownedCdp=true physicalInputVerified=false\n",multiwindow_companion_pid,browser_gpu_pid);
            return 0;
        }
        if(errno!=ENOENT) return -1;
        usleep(100000);
    }
    puts("OMARCHY_BROWSER_FAILURE stage=interaction-checkpoint");return -1;
}
static int browser_resize_checkpoint(unsigned index,unsigned width,unsigned height) {
    char command[256],request[160],temporary[192],body[128],proof_path[192],proof_body[1901];
    const unsigned before=atomic_load(&compositor_commits);
    const int n=snprintf(command,sizeof command,"hl.dispatch(hl.dsp.window.resize({x=%u,y=%u,window='class:omarchy-gpu-browser'}))",width,height);
    if(n<=0 || (size_t)n>=sizeof command) return -1;
    char* const resize[]={"/usr/bin/hyprctl","eval",command,NULL};
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    if(run_ctl(resize) || multiwindow_focus(1,98)) return -1;
    int observed=0;
    for(unsigned tick=0;tick<60;++tick) {
        if(run_ctl(active)) return -1;
        const char* size=strstr(last_control_reply,"\"size\"");unsigned w=0,h=0;
        if(size && sscanf(size,"\"size\" : [ %u , %u ]",&w,&h)==2 && w==width && h==height &&
           atomic_load(&compositor_commits)>=before+3) { observed=1;break; }
        usleep(50000);
    }
    if(!observed) return -1;
    scene_hex_receipt("OMARCHY_BROWSER_RESIZE_ACTIVE_HEX",last_control_reply,strlen(last_control_reply));
    const int a=snprintf(request,sizeof request,"/tmp/wddm-browser-resize-%d-%u",multiwindow_companion_pid,index);
    const int b=snprintf(temporary,sizeof temporary,"%s.tmp",request);
    const int bytes=snprintf(body,sizeof body,"%d:%d:%u:%u\n",multiwindow_companion_pid,browser_gpu_pid,width,height);
    const int c=snprintf(proof_path,sizeof proof_path,"/tmp/wddm-browser-resize-proof-%d-%u.json",multiwindow_companion_pid,index);
    if(a<=0 || (size_t)a>=sizeof request || b<=0 || (size_t)b>=sizeof temporary || bytes<=0 ||
       (size_t)bytes>=sizeof body || c<=0 || (size_t)c>=sizeof proof_path) return -1;
    const int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    const int valid=write(fd,body,(size_t)bytes)==bytes && !fchmod(fd,0400);
    const int closed=close(fd);if(!valid || closed || rename(temporary,request)) return -1;
    for(unsigned tick=0;tick<100;++tick) {
        if(browser_completed_failure()) return -1;
        const int proof=open(proof_path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(proof>=0) {
            struct stat status;
            const int valid_proof=!fstat(proof,&status) && S_ISREG(status.st_mode) && status.st_uid==getuid() &&
                status.st_nlink==1 && !(status.st_mode&0222) && status.st_size>0 && status.st_size<=1900;
            const ssize_t count=valid_proof ? read(proof,proof_body,sizeof proof_body) : -1;
            const int closed=close(proof);
            if(count<=0 || count!=status.st_size || closed) return -1;
            scene_hex_receipt("OMARCHY_BROWSER_RESIZE_PROOF_HEX",proof_body,(size_t)count);
            printf("OMARCHY_BROWSER_RESIZE_ACK browserPid=%d gpuPid=%d index=%u width=%u height=%u beforeCommit=%u observedCommit=%u\n",
                multiwindow_companion_pid,browser_gpu_pid,index,width,height,before,atomic_load(&compositor_commits));
            return 0;
        }
        if(errno!=ENOENT) return -1;
        usleep(100000);
    }
    puts("OMARCHY_BROWSER_FAILURE stage=browser-resize-gpu-proof");return -1;
}
static int visible_gtk_application_with_pid(const char* text,pid_t* pid) {
    if(pid) *pid=0;
    const char* match=strstr(text,"\"org.gnome.Nautilus\"");
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

static pid_t gtk_application_pid;
static int gtk_renderer_proof(void) {
    char path[128],body[1901],expected[64];
    const int n=snprintf(path,sizeof path,"/tmp/wddm-gtk-renderer-%d.json",gtk_application_pid);
    const int e=snprintf(expected,sizeof expected,"\"pid\":%d,\"uid\":1000,",gtk_application_pid);
    if(n<=0 || (size_t)n>=sizeof path || e<=0 || (size_t)e>=sizeof expected) return -1;
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return errno==ENOENT ? 0 : -1;
    struct stat status;
    const int valid=!fstat(fd,&status) && S_ISREG(status.st_mode) && status.st_uid==getuid() &&
        status.st_nlink==1 && !(status.st_mode&0222) && status.st_size>0 && status.st_size<=1900;
    const ssize_t count=valid ? read(fd,body,sizeof body-1) : -1;
    const int closed=close(fd);
    if(count<=0 || count!=status.st_size || closed) return -1;
    body[count]=0;scene_hex_receipt("OMARCHY_GTK_RENDERER_PROOF_HEX",body,(size_t)count);
    if(!strstr(body,expected) || !strstr(body,"\"mapped\":true") || !strstr(body,"\"realized\":true") ||
       !strstr(body,"\"glRenderer\":true")) return -1;
    return 1;
}
static int gtk_application_start(void) {
    char* const launch[]={"/usr/bin/hyprctl","eval",
        "hl.exec_cmd([[systemd-cat -t omarchy-gpu-companion -- env LD_PRELOAD=/opt/wddm-root/lib/gtk-renderer-probe.so:/linux-ioctl-bridge.so WDDM_GTK_RENDERER_PROBE=1 GDK_BACKEND=wayland GSK_RENDERER=gl GSK_DEBUG=renderer GDK_DEBUG=opengl /usr/bin/nautilus --new-window /usr/share/omarchy/applications]], {float=true,size={1000,800}})",NULL};
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    if(run_ctl(launch)) return -1;
    for(unsigned tick=0;tick<300;++tick) {
        if(run_ctl(clients)) return -1;
        pid_t pid=0;
        if(visible_gtk_application_with_pid(last_control_reply,&pid)) {
            gtk_application_pid=pid;
            const int ready=gtk_renderer_proof();
            if(ready<0) return -1;
            if(ready) {
                char* const place[]={"/usr/bin/hyprctl","eval",
                    "hl.dispatch(hl.dsp.window.float({action='enable',window='class:org.gnome.Nautilus'})); hl.dispatch(hl.dsp.window.resize({x=1000,y=800,window='class:org.gnome.Nautilus'})); hl.dispatch(hl.dsp.window.move({x=120,y=180,relative=false,window='class:org.gnome.Nautilus'})); hl.dispatch(hl.dsp.focus({window='class:org.gnome.Nautilus'}))",NULL};
                char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
                if(run_ctl(place) || run_ctl(active)) return -1;
                scene_hex_receipt("OMARCHY_GTK_READY_ACTIVE_HEX",last_control_reply,strlen(last_control_reply));
                printf("OMARCHY_GTK_APPLICATION_READY pid=%d class=org.gnome.Nautilus requestedWidth=1000 requestedHeight=800 requestedX=120 requestedY=180 nativeGpuProof=false\n",pid);
                return 0;
            }
        }
        usleep(100000);
    }
    puts("OMARCHY_GTK_FAILURE stage=window-or-realized-gl-renderer");return -1;
}
static int gtk_application_checkpoint(unsigned phase) {
    if(!gtk_application_pid && gtk_application_start()) return -1;
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    if(run_ctl(clients)) return -1;
    pid_t pid=0;
    if(!visible_gtk_application_with_pid(last_control_reply,&pid) || pid!=gtk_application_pid) return -1;
    scene_hex_receipt("OMARCHY_GTK_CHECKPOINT_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
    printf("OMARCHY_GTK_CHECKPOINT phase=%u pid=%d queuedFrames=%u nativeGpuProof=false\n",phase,pid,atomic_load(&queued_frames));
    return 0;
}
static int gtk_application_close(void) {
    if(gtk_application_pid<=0) return -1;
    char* const quit[]={"/usr/bin/hyprctl","eval","hl.exec_cmd([[env LD_PRELOAD= /usr/bin/nautilus --quit]])",NULL};
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    if(run_ctl(quit)) return -1;
    char path[128];const int n=snprintf(path,sizeof path,"/run/wddm-retired/%d",gtk_application_pid);
    if(n<=0 || (size_t)n>=sizeof path) return -1;
    for(unsigned tick=0;tick<120;++tick) {
        struct stat status;
        if(!lstat(path,&status) && S_ISREG(status.st_mode) && !status.st_uid && status.st_nlink==1 &&
           !(status.st_mode&0222) && status.st_size>0 && status.st_size<=24 &&
           kill(gtk_application_pid,0)<0 && errno==ESRCH) {
            if(run_ctl(clients) || strstr(last_control_reply,"\"org.gnome.Nautilus\"")) return -1;
            scene_hex_receipt("OMARCHY_GTK_CLOSED_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            printf("OMARCHY_GTK_APPLICATION_RETIRED pid=%d rootOwnedWitness=true processExited=true\n",gtk_application_pid);return 0;
        }
        usleep(100000);
    }
    puts("OMARCHY_GTK_FAILURE stage=live-native-retirement");return -1;
}

static int multiwindow_checkpoint(unsigned phase) {
    const unsigned before=atomic_load(&queued_frames);
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    if(run_ctl(clients) || browser_receipt()) return -1;
    scene_hex_receipt("OMARCHY_BROWSER_CHECKPOINT_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
    if(phase==1 && multiwindow_focus(1,99)) return -1;
    if((phase==1 || phase==3) && browser_interaction_checkpoint(phase)) return -1;
    if((phase==1 || phase==3) && (browser_resize_checkpoint(1,1600,1100) || browser_resize_checkpoint(2,1400,1000))) return -1;
    if(gtk_application_checkpoint(phase)) return -1;
    if(multiwindow_focus(1,phase) || multiwindow_focus(0,phase)) return -1;
    printf("OMARCHY_BROWSER_CHECKPOINT phase=%u browserPid=%d gpuPid=%d beforeQueued=%u afterQueued=%u physicalInputVerified=false\n",
        phase,multiwindow_companion_pid,browser_gpu_pid,before,atomic_load(&queued_frames));
    return 0;
}
static int browser_session_retired(void) {
    char path[128],expected[96],body[96];
    const int length=snprintf(path,sizeof path,"/tmp/wddm-browser-session-retired-%d",multiwindow_companion_pid);
    const int bytes=snprintf(expected,sizeof expected,"%d:%d\n",multiwindow_companion_pid,browser_gpu_pid);
    if(length<=0 || (size_t)length>=sizeof path || bytes<=0 || (size_t)bytes>=sizeof expected) return -1;
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return errno==ENOENT ? 0 : -1;
    struct stat status;
    if(fstat(fd,&status) || !S_ISREG(status.st_mode) || status.st_uid!=getuid() || status.st_nlink!=1 ||
       (status.st_mode&0222) || status.st_size!=bytes) { close(fd);return -1; }
    const ssize_t count=read(fd,body,sizeof body);const int closed=close(fd);
    if(count!=bytes || closed || memcmp(body,expected,(size_t)bytes)) return -1;
    return 1;
}
static int multiwindow_close(void) {
    char* const close_window[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.window.close({window='class:omarchy-gpu-browser'}))",NULL};
    if(multiwindow_companion_pid<=0 || browser_gpu_pid<=0 || run_ctl(close_window)) return -1;
    char path[96];const int count=snprintf(path,sizeof path,"/run/wddm-retired/%d",browser_gpu_pid);
    if(count<=0 || (size_t)count>=sizeof path) return -1;
    for(unsigned tick=0;tick<120;++tick) {
        struct stat status;
        if(!lstat(path,&status) && S_ISREG(status.st_mode) && !status.st_uid && status.st_nlink==1 &&
           !(status.st_mode&0222) && status.st_size>0 && status.st_size<=24 &&
           kill(multiwindow_companion_pid,0)<0 && errno==ESRCH) {
            const int retired=browser_session_retired();
            if(retired<0) return -1;
            if(!retired) { usleep(100000);continue; }
            char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
            if(run_ctl(clients) || strstr(last_control_reply,"\"omarchy-gpu-browser\"") || !visible_gpu_terminal(last_control_reply)) return -1;
            scene_hex_receipt("OMARCHY_BROWSER_CLOSED_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            printf("OMARCHY_BROWSER_GPU_RETIRED browserPid=%d gpuPid=%d rootOwnedWitness=true primaryStillMapped=true gpuProof=false\n",
                multiwindow_companion_pid,browser_gpu_pid);
            if(atomic_load(&stream_finished) && gtk_application_close()) return -1;
            return 0;
        }
        usleep(100000);
    }
    puts("OMARCHY_BROWSER_FAILURE stage=gpu-process-retirement");return -1;
}

static int multiwindow_failure_cleanup(void) {
    int failed=0;
    if(gtk_application_pid>0 && gtk_application_close()) failed=1;
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    char* const close_browser[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.window.close({window='class:omarchy-gpu-browser'}))",NULL};
    if(run_ctl(clients)) failed=1;
    else if(strstr(last_control_reply,"\"omarchy-gpu-browser\"")) {
        if(run_ctl(close_browser)) failed=1;
        int gone=0;
        for(unsigned tick=0;tick<200;++tick) {
            if(!run_ctl(clients) && !strstr(last_control_reply,"\"omarchy-gpu-browser\"")) { gone=1;break; }
            usleep(100000);
        }
        if(!gone) failed=1;
    }
    if(close_gpu_terminal(last_gpu_terminal_pid,1)) failed=1;
    printf("OMARCHY_FAILURE_APPLICATION_CLEANUP failed=%d originalWorkloadStillRejected=true\n",failed);
    return failed ? -1 : 0;
}
