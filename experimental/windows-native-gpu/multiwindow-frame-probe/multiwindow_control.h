/* SPDX-License-Identifier: MIT */
/* Explicit diagnostic controller; uses Hyprland IPC, not a Windows input hook. */
#pragma once
static pid_t multiwindow_companion_pid;

static int multiwindow_pulse(unsigned phase,int companion) {
    const char* path=companion?"/tmp/omarchy-companion-terminal-pulse.json":"/tmp/omarchy-active-terminal-pulse.json";
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return -1;
    struct stat status;char body[1025],label[96];
    if(fstat(fd,&status) || !S_ISREG(status.st_mode) || status.st_uid!=getuid() || status.st_nlink!=1 ||
       status.st_size<=0 || status.st_size>1024) { close(fd);return -1; }
    const ssize_t bytes=read(fd,body,sizeof body-1);const int closed=close(fd);
    if(bytes!=status.st_size || closed || bytes<=0) return -1;
    body[bytes]=0;
    const int count=snprintf(label,sizeof label,"OMARCHY_MULTIWINDOW_%u_%s_PULSE_HEX",phase,companion?"COMPANION":"PRIMARY");
    if(count<=0 || (size_t)count>=sizeof label) return -1;
    scene_hex_receipt(label,body,(size_t)bytes);return 0;
}

static int multiwindow_focus(int companion,unsigned phase) {
    char* const focus[]={"/usr/bin/hyprctl","eval",companion?
        "hl.dispatch(hl.dsp.focus({window='class:omarchy-gpu-companion'}))":
        "hl.dispatch(hl.dsp.focus({window='class:omarchy-gpu-terminal'}))",NULL};
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    if(run_ctl(focus) || run_ctl(active)) return -1;
    char label[96];const int count=snprintf(label,sizeof label,"OMARCHY_MULTIWINDOW_%u_%s_ACTIVE_HEX",phase,companion?"COMPANION":"PRIMARY");
    if(count<=0 || (size_t)count>=sizeof label) return -1;
    scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));return 0;
}

static int multiwindow_start(void) {
    if(unlink("/tmp/omarchy-companion-terminal-pulse.json") && errno!=ENOENT) return -1;
    char* const launch[]={"/usr/bin/hyprctl","eval",
        "hl.exec_cmd([[systemd-cat -t omarchy-gpu-companion -- /opt/wddm-root/bin/alacritty --config-file /opt/wddm-root/data/alacritty-desktop.toml -vv --class omarchy-gpu-companion --title 'NVIDIA companion' --hold -e /usr/bin/env -u LD_PRELOAD /opt/wddm-root/bin/companion-terminal-pulse]], {float=true, size={960,640}})",NULL};
    if(run_ctl(launch)) return -1;
    char* const active[]={"/usr/bin/hyprctl","-j","activewindow",NULL};
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    for(unsigned tick=0;tick<80;++tick) {
        if(run_ctl(active)) return -1;
        if(strstr(last_control_reply,"\"omarchy-gpu-companion\"") && !access("/tmp/omarchy-companion-terminal-pulse.json",R_OK)) {
            const char* field=strstr(last_control_reply,"\"pid\"");long value=0;char* end=NULL;
            if(!field || !(field=strchr(field,':'))) return -1;
            errno=0;value=strtol(field+1,&end,10);
            if(errno || end==field+1 || value<=0 || value>INT32_MAX) return -1;
            multiwindow_companion_pid=(pid_t)value;
            char* const place[]={"/usr/bin/hyprctl","eval",
                "hl.dispatch(hl.dsp.window.resize({x=960,y=640,window='class:omarchy-gpu-companion'})); hl.dispatch(hl.dsp.window.move({x=2800,y=200,relative=false,window='class:omarchy-gpu-companion'}))",NULL};
            if(run_ctl(place) || run_ctl(clients)) return -1;
            scene_hex_receipt("OMARCHY_MULTIWINDOW_READY_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            if(multiwindow_pulse(0,1) || multiwindow_focus(0,0)) return -1;
            printf("OMARCHY_MULTIWINDOW_READY primaryPid=%d companionPid=%d simultaneousWindows=2 queuedFrames=%u gpuProof=false\n",
                last_gpu_terminal_pid,multiwindow_companion_pid,atomic_load(&queued_frames));
            return 0;
        }
        usleep(100000);
    }
    puts("OMARCHY_MULTIWINDOW_FAILURE stage=companion-start");return -1;
}

static int multiwindow_checkpoint(unsigned phase) {
    const unsigned before=atomic_load(&queued_frames);
    char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
    if(run_ctl(clients)) return -1;
    char label[96];const int count=snprintf(label,sizeof label,"OMARCHY_MULTIWINDOW_%u_CLIENTS_HEX",phase);
    if(count<=0 || (size_t)count>=sizeof label) return -1;
    scene_hex_receipt(label,last_control_reply,strlen(last_control_reply));
    if(multiwindow_pulse(phase,0) || multiwindow_pulse(phase,1) ||
       multiwindow_focus(1,phase) || multiwindow_focus(0,phase)) return -1;
    printf("OMARCHY_MULTIWINDOW_CHECKPOINT phase=%u primaryPid=%d companionPid=%d beforeQueued=%u afterQueued=%u gpuProof=false physicalInputVerified=false\n",
        phase,last_gpu_terminal_pid,multiwindow_companion_pid,before,atomic_load(&queued_frames));
    return 0;
}

static int multiwindow_close(void) {
    char* const close_window[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.window.close({window='class:omarchy-gpu-companion'}))",NULL};
    if(multiwindow_companion_pid<=0 || run_ctl(close_window)) return -1;
    char path[96];const int count=snprintf(path,sizeof path,"/run/wddm-retired/%d",multiwindow_companion_pid);
    if(count<=0 || (size_t)count>=sizeof path) return -1;
    for(unsigned tick=0;tick<120;++tick) {
        struct stat status;
        if(!lstat(path,&status) && S_ISREG(status.st_mode) && !status.st_uid && status.st_nlink==1 &&
           !(status.st_mode&0222) && status.st_size>0 && status.st_size<=24) {
            FILE* file=fopen(path,"rb");if(!file) return -1;
            char body[32]={0};const size_t bytes=fread(body,1,sizeof body-1,file);
            const int bad=ferror(file),closed=fclose(file);if(bad || closed || !bytes) return -1;
            char* end=NULL;errno=0;const unsigned long long actor=strtoull(body,&end,10);
            if(errno || !actor || end==body || strcmp(end,"\n")) return -1;
            char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
            if(run_ctl(clients) || strstr(last_control_reply,"\"omarchy-gpu-companion\"") || !visible_gpu_terminal(last_control_reply)) return -1;
            scene_hex_receipt("OMARCHY_MULTIWINDOW_CLOSED_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            printf("OMARCHY_MULTIWINDOW_COMPANION_RETIRED pid=%d actor=%llu primaryStillMapped=true compositorStillRunning=true rootOwnedWitness=true gpuProof=false\n",
                multiwindow_companion_pid,actor);return 0;
        }
        usleep(100000);
    }
    puts("OMARCHY_MULTIWINDOW_FAILURE stage=companion-retirement");return -1;
}
