// SPDX-License-Identifier: MIT
// Supervised guest-only graphics service for the read-only normal-root test.
// Reuse the tested module/broker teardown; never call the legacy PID 1 main.
#define main wddm_unused_legacy_diagnostic_main
#include "runtime_guest_init.c"
#undef main
#include <pwd.h>
#include <stdint.h>
#include <sys/sysmacros.h>
#include "guest_session_process.h"

static int uwsm_file(const char* path,const char* text) {
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0) return -1;
    const size_t size=strlen(text);
    const int written=write(fd,text,size)==(ssize_t)size;
    const int closed=close(fd)==0;
    return written && closed ? 0 : -1;
}
static int uwsm_prepare(unsigned uid,unsigned gid) {
    // The original direct fixture inherits this dedicated guest console.
    // Give the UWSM user unit the same synchronous output path: journald's
    // asynchronous relay cannot prove receipt order across GPU processes.
    const int console=open("/dev/ttyS0",O_WRONLY|O_CLOEXEC|O_NOFOLLOW|O_NOCTTY);
    struct stat device;
    if(console<0) return -1;
    const int granted=!fstat(console,&device) && S_ISCHR(device.st_mode) && device.st_rdev==makedev(4,64) &&
        !fchown(console,uid,gid) && !fchmod(console,0600);
    close(console);if(!granted) return -1;
    if(mkdir("/etc/systemd/user/wayland-wm@wddmprobe.desktop.service.d",0755)) return -1;
    return uwsm_file("/etc/systemd/user/wayland-wm@wddmprobe.desktop.service.d/70-wddm-probe.conf",
        "[Service]\nNoNewPrivileges=yes\nStandardOutput=tty\nStandardError=tty\n"
        "TTYPath=/dev/ttyS0\nTTYReset=no\nTTYVHangup=no\n") ||
        uwsm_file("/usr/share/wayland-sessions/wddmprobe.desktop",
        "[Desktop Entry]\nName=Owned NVIDIA Hyprland integration probe\nType=Application\n"
        "DesktopNames=Hyprland\nExec=/opt/wddm-root/bin/uwsm-compositor-exec\n");
}
static int uwsm_environment_value(FILE* output,const char* name,const char* value) {
    if(!value) return 0;
    // Only fixed supervisor-selected variables are serialized. Values are not
    // shell code; disallow quoting/line syntax rather than evaluating anything.
    if(strpbrk(value,"\r\n\"\\")) return -1;
    return fprintf(output,"%s=\"%s\"\n",name,value)<0 ? -1 : 0;
}
static int uwsm_environment(const char* variables[][2],size_t count) {
    const int fd=open("/run/wddm-broker/compositor.env",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0644);
    if(fd<0) return -1;
    FILE* output=fdopen(fd,"w");if(!output) { close(fd);return -1; }
    int result=0;
    for(size_t index=0;index<count;++index)
        if(uwsm_environment_value(output,variables[index][0],getenv(variables[index][0]))) result=-1;
    const char* names[]={"WDDM_BRIDGE_LINUX_UMD_NAME","WDDM_BRIDGE_NORMAL_DESKTOP_PROBE","DBUS_SESSION_BUS_ADDRESS","OMARCHY_PATH",
        "WDDM_BRIDGE_DESKTOP_4K_PROBE","WDDM_BRIDGE_DESKTOP_4K_CPU_PROBE","WDDM_BRIDGE_DESKTOP_SPARSE_PROBE",
        "WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE","WDDM_BRIDGE_ACTOR_PROFILE","QSG_RHI_BACKEND","QSG_INFO",
        "WDDM_BRIDGE_DESKTOP_CONTENT","WDDM_BRIDGE_DESKTOP_RETIREMENT","WDDM_BRIDGE_DESKTOP_LARGE_MAP_PROBE",
        "WAYLAND_DEBUG","WDDM_BRIDGE_BROKER_SOCKET","XDG_RUNTIME_DIR","WDDM_BRIDGE_UWSM_PROBE","WDDM_BRIDGE_NATIVE_INPUT"};
    for(size_t index=0;index<sizeof names/sizeof names[0];++index)
        if(uwsm_environment_value(output,names[index],getenv(names[index]))) result=-1;
    // EGL_PLATFORM was deliberately unset for coordinated Wayland clients.
    if(fflush(output) || fchmod(fd,0644)) result=-1;
    if(fclose(output)) result=-1;
    return result;
}
static int uwsm_launcher_environment(const char* runtime) {
    if(clearenv()) return -1;
    const char* values[][2]={{"HOME","/home/omarchy"},{"USER","omarchy"},{"LOGNAME","omarchy"},
        {"SHELL","/bin/bash"},{"PATH","/usr/bin:/usr/local/bin"},{"LC_ALL","C.UTF-8"},
        {"XDG_RUNTIME_DIR",runtime},{"DBUS_SESSION_BUS_ADDRESS","unix:path=/run/user/1000/bus"},
        {"OMARCHY_PATH","/usr/share/omarchy"}};
    for(size_t index=0;index<sizeof values/sizeof values[0];++index)
        if(setenv(values[index][0],values[index][1],1)) return -1;
    return 0;
}
static void uwsm_inspect(unsigned uid,unsigned gid,int complete) {
    // Preserve the actual unit state and bounded guest journal before RAM-only
    // shutdown. A missing compositor is a failed run, never an admission bypass.
    for(unsigned journal=0;journal<(complete ? 3u : 1u);++journal) {
        const pid_t query=fork();
        if(!query) {
            if(journal==2) {
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","-n","40","-o","short-monotonic",
                    "-u","wddm-uwsm-login.service","-u","systemd-logind.service",(char*)NULL);_exit(127);
            }
            if(journal==1) {
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","-n","80","-o","short-monotonic","_UID=1000",(char*)NULL);_exit(127);
            }
            if(uwsm_launcher_environment("/run/user/1000") || setgroups(0,NULL) || setgid(gid) || setuid(uid)) _exit(126);
            execl("/usr/bin/systemctl","systemctl","--user","show","wayland-wm@wddmprobe.desktop.service",
                "wayland-wm-env@wddmprobe.desktop.service","graphical-session.target","--no-pager",
                "--property=Id,ActiveState,SubState,MainPID,ControlGroup,Result,ExecMainStatus,NoNewPrivileges",(char*)NULL);_exit(127);
        }
        if(query>0) {
            alarm(5);int status=0;
            if(waitpid(query,&status,0)!=query) { kill(query,SIGKILL);waitpid(query,NULL,0); }
            alarm(0);
        }
    }
}
static int uwsm_graphical_active(unsigned uid,unsigned gid) {
    const pid_t query=fork();
    if(!query) {
        if(uwsm_launcher_environment("/run/user/1000") || setgroups(0,NULL) || setgid(gid) || setuid(uid)) _exit(126);
        execl("/usr/bin/systemctl","systemctl","--user","--quiet","is-active","graphical-session.target",(char*)NULL);_exit(127);
    }
    if(query<0) return 0;
    alarm(3);int status=0;
    const int finished=waitpid(query,&status,0)==query;
    if(!finished) { kill(query,SIGKILL);waitpid(query,NULL,0); }
    alarm(0);
    return finished && WIFEXITED(status) && !WEXITSTATUS(status);
}

// Preserve a failed owned compositor's report before its RAM overlay disappears.
// The PID comes from waitpid, not guest input; never follow a report symlink.
static void compositor_crash_report(int trace,pid_t child,uid_t owner) {
    char path[160];
    const int path_size=snprintf(path,sizeof path,"/home/omarchy/.cache/hyprland/hyprlandCrashReport%ld.txt",(long)child);
    if(path_size<=0 || (size_t)path_size>=sizeof path) return;
    const int report=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    struct stat metadata;
    if(report<0) {
        dprintf(trace,"\nNORMAL_ROOT_GRAPHICS_CRASH_REPORT_UNAVAILABLE pid=%ld errno=%d\n",(long)child,errno);
        return;
    }
    if(fstat(report,&metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid!=owner ||
       metadata.st_nlink!=1 || metadata.st_size<=0) {
        close(report);
        dprintf(trace,"\nNORMAL_ROOT_GRAPHICS_CRASH_REPORT_REJECTED pid=%ld\n",(long)child);
        return;
    }
    const size_t limit=96u*1024u;
    const size_t bytes=(uintmax_t)metadata.st_size>limit ? limit : (size_t)metadata.st_size;
    dprintf(trace,"\nNORMAL_ROOT_GRAPHICS_CRASH_REPORT_BEGIN pid=%ld bytes=%zu truncated=%s owner=%u\n",
        (long)child,bytes,(uintmax_t)metadata.st_size>limit ? "true" : "false",(unsigned)owner);
    size_t offset=0;unsigned index=0;
    while(offset<bytes) {
        unsigned char raw[1536];char line[sizeof raw*2+128];
        const size_t requested=bytes-offset<sizeof raw ? bytes-offset : sizeof raw;
        ssize_t count;
        do { count=read(report,raw,requested); } while(count<0 && errno==EINTR);
        if(count<=0) break;
        const int prefix=snprintf(line,sizeof line,"\nNORMAL_ROOT_GRAPHICS_CRASH_REPORT_HEX pid=%ld index=%u data=",(long)child,index);
        if(prefix<=0 || (size_t)prefix+(size_t)count*2+1>sizeof line) break;
        static const char hex[]="0123456789abcdef";
        for(size_t byte=0;byte<(size_t)count;++byte) {
            line[prefix+byte*2]=hex[raw[byte]>>4];line[prefix+byte*2+1]=hex[raw[byte]&15];
        }
        const size_t size=(size_t)prefix+(size_t)count*2;line[size]='\n';
        ssize_t written;
        do { written=write(trace,line,size+1); } while(written<0 && errno==EINTR);
        if(written!=(ssize_t)(size+1)) break;
        offset+=(size_t)count;++index;
    }
    close(report);
    dprintf(trace,"\nNORMAL_ROOT_GRAPHICS_CRASH_REPORT_END pid=%ld chunks=%u bytes=%zu complete=%s\n",
        (long)child,index,offset,offset==bytes ? "true" : "false");
}

int main(void) {
    if(getpid()==1 || getuid() || geteuid() || access("/opt/wddm-root/normal-root-graphics",R_OK)) return 125;
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    struct rlimit core={0,0};
    if(setrlimit(RLIMIT_CORE,&core)) { perror("disable experimental renderer core dumps");return 125; }
    struct passwd* user=getpwnam("omarchy");
    if(!user || user->pw_uid!=1000 || !user->pw_gid || strcmp(user->pw_dir,"/home/omarchy")) return 125;
    const unsigned uid=user->pw_uid,gid=user->pw_gid;
    const int desktop=access("/opt/wddm-root/normal-desktop-probe",R_OK)==0;
    const int uwsm=access("/opt/wddm-root/uwsm-desktop-probe",R_OK)==0;
    if(uwsm && (!desktop || access("/opt/wddm-root/desktop-retirement-probe",R_OK) || uwsm_prepare(uid,gid))) return 125;
    const char* port="/dev/vport0p1";
    puts("NORMAL_ROOT_GRAPHICS_BEGIN normalSystemdService=true normalUid=1000 diagnosticCompositor=true normalOmarchySession=false");
    if(desktop) {
        // Start the normal user manager and session bus in this disposable OS.
        const pid_t manager=fork();
        if(!manager) { execl("/usr/bin/systemctl","systemctl","start","user@1000.service",(char*)NULL);_exit(127); }
        int status=0;
        if(manager<0 || waitpid(manager,&status,0)!=manager || !WIFEXITED(status) || WEXITSTATUS(status)) return 125;
        printf("OMARCHY_DESKTOP_PROBE_BEGIN normalConfig=true normalUserBus=true uwsmProbe=%s normalOmarchySession=false\n",uwsm ? "true" : "false");
    }
    if(desktop && !access("/opt/wddm-root/desktop-content-probe",R_OK)) {
        // This install writes only the owned RAM overlay. Decode the shipped
        // 6K wallpaper at the actual 4K output size before its GPU upload.
        const pid_t wallpaper=fork();
        if(!wallpaper) {
            execl("/usr/bin/install","install","-m","0644","/opt/wddm-root/data/Background.qml",
                "/usr/share/omarchy/shell/plugins/background/Background.qml",(char*)NULL);_exit(127);
        }
        int installed=0;
        if(wallpaper<0 || waitpid(wallpaper,&installed,0)!=wallpaper || !WIFEXITED(installed) || WEXITSTATUS(installed)) return 125;
        puts("OMARCHY_WALLPAPER_DECODE width=3840 height=2160 sourceImageUnchanged=true ownedRamOverlayOnly=true gpuProof=false");
        const pid_t setup=fork();
        if(!setup) {
            if(setgroups(0,NULL) || setgid(gid) || setuid(uid) || setenv("HOME","/home/omarchy",1)) _exit(126);
            execl("/bin/bash","bash","-eu","-c",
                "umask 077; current=$HOME/.local/state/omarchy/current; mkdir -p \"$current\" \"$HOME/.cache/hyprland\"; "
                "test -f /usr/share/omarchy/themes/tokyo-night/backgrounds/0-winding-road.jpg; "
                "ln -sfn /usr/share/omarchy/themes/tokyo-night \"$current/theme\"; "
                "printf 'tokyo-night\\n' > \"$current/theme.name\"; "
                "ln -sfn /usr/share/omarchy/themes/tokyo-night/backgrounds/0-winding-road.jpg \"$current/background\"",(char*)NULL);
            _exit(127);
        }
        int status=0;
        if(setup<0 || waitpid(setup,&status,0)!=setup || !WIFEXITED(status) || WEXITSTATUS(status)) return 125;
        puts("OMARCHY_DESKTOP_CONTENT_SETUP ordinaryUid=1000 shippedTheme=tokyo-night shippedWallpaper=0-winding-road.jpg ownedRamOverlayOnly=true");
    }
    for(unsigned tick=0;tick<200 && access(port,F_OK);++tick) usleep(50000);
    if(chmod(port,0600)) { perror("native port permissions");return 125; }
    int result=125;
    pid_t shell_trace=-1,session_status=-1,compositor_pid=-1;
    const int aperture=broker_peer_start() ? -1 : kernel_aperture_start();
    const int render=aperture>0 ? native_render_start() : -1;
    const pid_t broker=aperture>0 && render>0 ? graphics_broker_start_as(port,uid,gid) : -1;
    const char* runtime="/run/user/1000";
    if((mkdir(runtime,0700) && errno!=EEXIST) || chown(runtime,uid,gid) || chmod(runtime,0700)) {
        perror("ordinary user runtime");goto cleanup;
    }
    if(broker>0) {
        const pid_t child=fork();
        if(!child) {
            const char* variables[][2]={{"WDDM_BRIDGE_RUNTIME_WORKLOAD","opengl"},
                {"EGL_PLATFORM","surfaceless"},{"GALLIUM_DRIVER","d3d12"},{"MESA_D3D12_DEFAULT_ADAPTER_NAME","NVIDIA"},
                {"LIBGL_DRIVERS_PATH","/opt/wddm-mesa/lib/dri"},{"MESA_SHADER_CACHE_DISABLE","true"},{"EGL_LOG_LEVEL","debug"},
                {"WDDM_BRIDGE_KERNEL_APERTURE","1"},{"WDDM_BRIDGE_RENDER_NODE",native_render_path},
                {"LD_PRELOAD","/linux-ioctl-bridge.so:/hyprland-startup-hook.so"},
                {"LD_LIBRARY_PATH","/opt/wddm-mesa/lib:/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu"},
                {"WDDM_BRIDGE_HYPRLAND_RUNTIME",runtime},{"WDDM_BRIDGE_HYPRLAND_HOME","/home/omarchy"},
                {"FONTCONFIG_FILE","/opt/wddm-root/data/compositor-fonts.conf"},{"LC_ALL","C.UTF-8"},
                {"HOME","/home/omarchy"},{"USER","omarchy"},{"LOGNAME","omarchy"},{"SHELL","/bin/bash"},
                {"PATH","/usr/bin:/usr/local/bin"}};
            FILE* name=fopen("/linux-umd-name","rb");char module[256]={0};
            if(!name || !fread(module,1,sizeof(module)-1,name)) _exit(125);
            fclose(name);module[strcspn(module,"\r\n")]=0;
            if(setenv("WDDM_BRIDGE_LINUX_UMD_NAME",module,1)) _exit(125);
            if(desktop && (setenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE","1",1) ||
                setenv("DBUS_SESSION_BUS_ADDRESS","unix:path=/run/user/1000/bus",1) ||
                setenv("OMARCHY_PATH","/usr/share/omarchy",1))) _exit(125);
            if(desktop && !access("/opt/wddm-root/normal-desktop-shell-software",R_OK)) {
                if(setenv("QT_QUICK_BACKEND","software",1)) _exit(125);
                puts("OMARCHY_DESKTOP_SHELL_RENDERER qtQuickSoftware=true compositorNvidia=true productAcceptance=false");
            }
            if(desktop && !access("/opt/wddm-root/desktop-4k-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_4K_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-4k-cpu-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_4K_CPU_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-sparse-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_SPARSE_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-presentation-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-coordinator-probe",R_OK)) {
                if(setenv("WDDM_BRIDGE_ACTOR_PROFILE",!access("/opt/wddm-root/desktop-reference-probe",R_OK) ? "13" : !access("/opt/wddm-root/desktop-slot-probe",R_OK) ? "12" : !access("/opt/wddm-root/desktop-fence-probe",R_OK) ? "11" : "10",1) || setenv("QSG_RHI_BACKEND","opengl",1) ||
                   setenv("QSG_INFO","1",1)) _exit(125);
            }
            if(desktop && !access("/opt/wddm-root/desktop-content-probe",R_OK) && setenv("WDDM_BRIDGE_DESKTOP_CONTENT","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-retirement-probe",R_OK) && setenv("WDDM_BRIDGE_DESKTOP_RETIREMENT","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/native-input-probe",R_OK) && setenv("WDDM_BRIDGE_NATIVE_INPUT","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-protocol-trace",R_OK) && setenv("WAYLAND_DEBUG","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-large-map-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_LARGE_MAP_PROBE","1",1)) _exit(125);
            for(unsigned index=0;index<sizeof(variables)/sizeof(variables[0]);++index)
                if(setenv(variables[index][0],variables[index][1],1)) _exit(125);
            if(!access("/opt/wddm-root/desktop-coordinator-probe",R_OK) && unsetenv("EGL_PLATFORM")) _exit(125);
            if(uwsm) {
                if(setenv("WDDM_BRIDGE_BROKER_SOCKET","/run/wddm-broker/graphics.sock",1) ||
                   setenv("XDG_RUNTIME_DIR",runtime,1) || setenv("WDDM_BRIDGE_UWSM_PROBE","1",1) ||
                   uwsm_environment(variables,sizeof variables/sizeof variables[0]) || uwsm_launcher_environment(runtime)) _exit(125);
                if(graphics_primary_writer>=0) { close(graphics_primary_writer);graphics_primary_writer=-1; }
                // A user manager alone is not a login session. Ask the guest's
                // ordinary PAM stack/logind to create one; never fabricate an
                // XDG_SESSION_ID to get past UWSM's session validation.
                execl("/usr/bin/systemd-run","systemd-run","--unit=wddm-uwsm-login","--wait","--collect",
                    "--property=User=omarchy","--property=Group=omarchy","--property=PAMName=login",
                    "--property=TTYPath=/dev/tty1","--property=StandardInput=tty","--property=NoNewPrivileges=yes",
                    "--property=TTYReset=yes",
                    "--property=StandardOutput=journal","--property=StandardError=journal","--property=SyslogIdentifier=wddm-uwsm-login",
                    "--property=RuntimeMaxSec=240","--property=TimeoutStopSec=5","--property=KillMode=mixed",
                    "--setenv=HOME=/home/omarchy","--setenv=USER=omarchy","--setenv=LOGNAME=omarchy",
                    "--setenv=SHELL=/bin/bash","--setenv=PATH=/usr/bin:/usr/local/bin","--setenv=LC_ALL=C.UTF-8",
                    "--setenv=XDG_RUNTIME_DIR=/run/user/1000","--setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus",
                    "/usr/bin/uwsm","start","-g","-1","-e","-D","Hyprland","wddmprobe.desktop",(char*)NULL);
                perror("PAM UWSM session exec");_exit(127);
            }
            if(graphics_client_drop_as(port,uid,gid)) { perror("normal graphics privilege drop");_exit(126); }
            execl("/guest-probe","/guest-probe",(char*)NULL);perror("normal graphics compositor exec");_exit(127);
        }
        if(child>0) {
            shell_trace=fork();
            if(!shell_trace) {
                execl("/usr/bin/journalctl","journalctl","-b","-t","omarchy-shell","-t","omarchy-gpu-app","-t","wddm-uwsm-login","--no-pager","-f","-n","100",(char*)NULL);_exit(127);
            }
            if(graphics_primary_writer>=0) {
                pid_t primary=child;int process=-1;
                if(uwsm) {
                    const char* group="/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@wddmprobe.desktop.service";
                    for(unsigned attempt=0;attempt<400;++attempt) {
                        process=wddm_session_find("/guest-probe",group,uid,gid,&primary);
                        if(process>=0 || errno!=EAGAIN) break;
                        usleep(50000);
                    }
                    printf("NORMAL_ROOT_UWSM_PRIMARY pid=%ld launcherPid=%ld exactExecutable=true exactCgroup=true pinnedPidfd=%s normalOmarchySession=false\n",
                        (long)(process>=0 ? primary : 0),(long)child,process>=0 ? "true" : "false");
                    uwsm_inspect(uid,gid,1);
                } else process=(int)syscall(SYS_pidfd_open,child,0);
                const int registered=process>=0 ? graphics_primary_register(primary,process) : -1;
                if(process>=0) close(process);
                if(registered) { kill(child,SIGKILL);waitpid(child,NULL,0);goto cleanup; }
                compositor_pid=primary;
                if(uwsm) {
                    session_status=fork();
                    if(!session_status) {
                        for(unsigned attempt=0;attempt<30;++attempt) {
                            if(uwsm_graphical_active(uid,gid)) break;
                            sleep(1);
                        }
                        printf("\nNORMAL_ROOT_UWSM_STATE_BEGIN expectedPid=%ld phase=running\n",(long)primary);
                        uwsm_inspect(uid,gid,0);
                        puts("NORMAL_ROOT_UWSM_STATE_END phase=running");
                        _exit(0);
                    }
                }
            }
            // Full-resolution initialization + explicit cleanup must fit a
            // separately bounded diagnostic window. Older probes keep 45 s.
            const unsigned deadline=access("/opt/wddm-root/desktop-fence-probe",R_OK)==0 ? 240u : access("/opt/wddm-root/desktop-coordinator-probe",R_OK)==0 ? 120u : access("/opt/wddm-root/desktop-presentation-probe",R_OK)==0 ? 90u : 45u;
            printf("NORMAL_ROOT_GRAPHICS_SUPERVISION deadlineSeconds=%u\n",deadline);
            struct sigaction action={0};action.sa_handler=timeout_handler;sigaction(SIGALRM,&action,NULL);alarm(deadline);
            int status=0;
            if(waitpid(child,&status,0)==child) {
                result=WIFEXITED(status) ? WEXITSTATUS(status) : 128;
                if(WIFSIGNALED(status)) {
                    // A tty hangup can invalidate inherited stdout. Reopen
                    // the console for the supervisor's diagnostic receipt.
                    const int trace=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);
                    if(trace>=0) {
                        dprintf(trace,"NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=%d coreDumped=%s\n",WTERMSIG(status),WCOREDUMP(status) ? "true" : "false");
                        compositor_crash_report(trace,child,(uid_t)uid);
                        close(trace);
                    }
                }
            }
            else { kill(child,SIGKILL);waitpid(child,NULL,0);result=124; }
            alarm(0);
        }
    }
cleanup:
    if(uwsm && compositor_pid>0) {
        char path[160];struct stat metadata;
        const int bytes=snprintf(path,sizeof path,"/home/omarchy/.cache/hyprland/hyprlandCrashReport%ld.txt",(long)compositor_pid);
        if(bytes>0 && (size_t)bytes<sizeof path && !lstat(path,&metadata) && S_ISREG(metadata.st_mode) &&
            metadata.st_uid==uid && metadata.st_nlink==1 && metadata.st_size>0) {
            const int trace=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);
            if(trace>=0) { compositor_crash_report(trace,compositor_pid,(uid_t)uid);close(trace); }
            result=125;
        }
    }
    if(shell_trace>0) { kill(shell_trace,SIGTERM);waitpid(shell_trace,NULL,0); }
    if(session_status>0) {
        if(waitpid(session_status,NULL,WNOHANG)!=session_status) { kill(session_status,SIGTERM);waitpid(session_status,NULL,0); }
    }
    if(uwsm) {
        const pid_t stop=fork();
        if(!stop) { execl("/usr/bin/systemctl","systemctl","stop","wddm-uwsm-login.service",(char*)NULL);_exit(127); }
        if(stop>0) waitpid(stop,NULL,0);
    }
    if(desktop && !access("/opt/wddm-root/desktop-coordinator-probe",R_OK)) {
        const pid_t stop=fork();
        if(!stop) { execl("/usr/bin/systemctl","systemctl","stop","user@1000.service",(char*)NULL);_exit(127); }
        int status=0;
        const int stopped=stop>0 && waitpid(stop,&status,0)==stop && WIFEXITED(status) && !WEXITSTATUS(status);
        if(!stopped) result=125;
        printf("NORMAL_ROOT_COORDINATOR_USER_STOP ordinaryUserManagerStopped=%s ownedGuestOnly=true\n",stopped ? "true" : "false");
    }
    if(graphics_broker_wait(broker)) result=125;
    if(desktop) {
        // Replay only the owned applications' renderer identities once their
        // trace writers have stopped. Keep journal metadata, including PID,
        // so verification can bind each renderer to its authenticated peer.
        if(write(STDOUT_FILENO,"\n",1)!=1) result=125;
        const pid_t journal=fork();
        if(!journal) {
            execl("/usr/bin/journalctl","journalctl","-b","-t","omarchy-shell","-t","omarchy-gpu-app",
                "--no-pager","-n","64","--grep",
                "OpenGL VENDOR|Created QRhi|Running on D3D12|Using OpenGL 3.3 renderer",(char*)NULL);
            _exit(127);
        }
        if(journal>0) waitpid(journal,NULL,0);
    }
    if(native_render_loaded && native_render_stop()) result=125;
    if(aperture>0 && kernel_aperture_stop()) result=125;
    printf("NORMAL_ROOT_GRAPHICS_EXIT result=%d normalOmarchySession=false\n",result);
    sync();return result;
}
