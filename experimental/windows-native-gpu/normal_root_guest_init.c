// SPDX-License-Identifier: MIT
// Guest-only PID 1: read-only image plus a bounded, temporary RAM overlay.
// This boot prerequisite does not claim graphics or desktop acceptance.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>

static void fail(const char* operation) {
    const int error=errno;
    dprintf(STDERR_FILENO,"NORMAL_ROOT_BOOT_FAILURE operation=%s errno=%d\n",operation,error);
    sync(); reboot(RB_POWER_OFF);
    for (;;) pause();
}
static void directory(const char* path,mode_t mode) {
    if (mkdir(path,mode) && errno!=EEXIST) fail(path);
}
static void write_new(const char* path,const char* text,mode_t mode) {
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,mode);
    if(fd<0) fail(path);
    for(size_t done=0,bytes=strlen(text);done<bytes;) {
        const ssize_t count=write(fd,text+done,bytes-done);
        if(count<0 && errno==EINTR) continue;
        if(count<=0) fail("overlay-file-write");
        done+=(size_t)count;
    }
    if(close(fd)) fail("overlay-file-close");
}
#ifndef WDDM_NORMAL_SESSION_BOOTSTRAP
static uint64_t copied_bytes;
static void copy_bundle(int source,int target,unsigned depth) {
    if(depth>12) { errno=E2BIG;fail("bundle-depth"); }
    DIR* entries=fdopendir(dup(source));if(!entries) fail("bundle-list");
    struct dirent* entry;
    while((entry=readdir(entries))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        struct stat state;
        if(fstatat(source,entry->d_name,&state,AT_SYMLINK_NOFOLLOW)) fail("bundle-stat");
        if(S_ISDIR(state.st_mode)) {
            if(mkdirat(target,entry->d_name,0755)) fail("bundle-directory");
            const int a=openat(source,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            const int b=openat(target,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(a<0 || b<0) fail("bundle-directory-open");
            copy_bundle(a,b,depth+1);
            if(close(a) || close(b)) fail("bundle-directory-close");
        } else if(S_ISREG(state.st_mode)) {
            if(state.st_size<0 || (uint64_t)state.st_size>384ull*1024*1024-copied_bytes) { errno=EFBIG;fail("bundle-byte-quota"); }
            const int a=openat(source,entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            const int b=openat(target,entry->d_name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,state.st_mode&0777);
            if(a<0 || b<0) fail("bundle-file-open");
            char buffer[65536];uint64_t total=0;
            for(;;) {
                ssize_t count=read(a,buffer,sizeof buffer);
                if(count<0 && errno==EINTR) continue;
                if(count<0) fail("bundle-file-read");
                if(!count) break;
                total+=(uint64_t)count;
                if(total>(uint64_t)state.st_size) { errno=EFBIG;fail("bundle-file-grew"); }
                for(ssize_t done=0;done<count;) {
                    const ssize_t written=write(b,buffer+done,(size_t)(count-done));
                    if(written<0 && errno==EINTR) continue;
                    if(written<=0) fail("bundle-file-write");
                    done+=written;
                }
            }
            if(total!=(uint64_t)state.st_size || close(a) || close(b)) fail("bundle-file-size-close");
            copied_bytes+=total;
        } else { errno=EINVAL;fail("bundle-file-type"); }
    }
    if(closedir(entries)) fail("bundle-list-close");
}
static void graphics_payload(void) {
    directory("/newroot/opt",0755);directory("/newroot/opt/wddm-root",0755);
    const int source=open("/normal-root-overlay",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    const int target=open("/newroot/opt/wddm-root",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(source<0 || target<0) fail("bundle-root-open");
    copy_bundle(source,target,0);
    if(close(source) || close(target)) fail("bundle-root-close");
    directory("/newroot/usr/lib/wsl",0755);
    const char* links[][2]={{"/usr/lib/x86_64-linux-gnu","/opt/wddm-root/lib"},
        {"/usr/lib/wsl/lib","/opt/wddm-root/wsl-libs"},{"/usr/lib/wsl/drivers","/opt/wddm-root/wsl-drivers"},
        {"/opt/wddm-mesa","/opt/wddm-root/mesa"},
        {"/guest-probe","/usr/bin/Hyprland"},{"/guest-graphics-broker","/opt/wddm-root/bin/guest-graphics-broker"},
        {"/aperture-contract","/opt/wddm-root/bin/aperture-contract"},
        {"/linux-ioctl-bridge.so","/opt/wddm-root/lib/linux-ioctl-bridge.so"},
        {"/hyprland-startup-hook.so","/opt/wddm-root/lib/hyprland-startup-hook.so"},
        {"/wddm-aperture.ko","/opt/wddm-root/kernel/wddm-aperture.ko"},{"/wddm-render.ko","/opt/wddm-root/kernel/wddm-render.ko"},
        {"/linux-umd-name","/opt/wddm-root/data/linux-umd-name"},{"/hyprland.conf","/opt/wddm-root/data/hyprland.conf"},
        {"/foot.ini","/opt/wddm-root/data/foot.ini"},{"/desktop-scene.sh","/opt/wddm-root/data/desktop-scene.sh"},
        {"/graphics-broker-required","/opt/wddm-root/normal-root-graphics"},
        {"/kernel-aperture-required","/opt/wddm-root/normal-root-graphics"},{"/native-sync-files-required","/opt/wddm-root/normal-root-graphics"},
        {"/native-buffers-required","/opt/wddm-root/normal-root-graphics"},{"/native-render-required","/opt/wddm-root/normal-root-graphics"}};
    for(unsigned index=0;index<sizeof(links)/sizeof(links[0]);++index) {
        char path[256];
        if(snprintf(path,sizeof path,"/newroot%s",links[index][0])>=(int)sizeof path || symlink(links[index][1],path)) fail("bundle-root-alias");
    }
    directory("/newroot/etc/systemd/system",0755);directory("/newroot/etc/systemd/system/multi-user.target.wants",0755);
    if(!access("/newroot/opt/wddm-root/mask-serial-getty",F_OK)) {
        if(symlink("/dev/null","/newroot/etc/systemd/system/serial-getty@ttyS0.service")) fail("dedicated-graphics-console");
        puts("NORMAL_ROOT_SERIAL_CONSOLE dedicatedToGraphics=true serialGettyMaskedInRam=true");
    }
    if(!access("/newroot/opt/wddm-root/uwsm-desktop-probe",F_OK)) {
        // Reserve this disposable guest's first VT for the real PAM session.
        // The persistent image and the ordinary launcher are untouched.
        if(symlink("/dev/null","/newroot/etc/systemd/system/getty@tty1.service")) fail("dedicated-uwsm-login-vt");
        directory("/newroot/etc/systemd/system/omarchy-provision-owner.service.d",0755);
        write_new("/newroot/etc/systemd/system/omarchy-provision-owner.service.d/90-wddm-probe.conf",
            "[Unit]\nConditionPathExists=!/opt/wddm-root/uwsm-desktop-probe\n",0644);
        directory("/newroot/etc/systemd/system/display-manager.service.d",0755);
        write_new("/newroot/etc/systemd/system/display-manager.service.d/90-wddm-probe.conf",
            "[Unit]\nConditionPathExists=!/opt/wddm-root/uwsm-desktop-probe\n",0644);
        puts("NORMAL_ROOT_UWSM_VT terminal=tty1 competingGettyMaskedInRam=true realPamSessionRequired=true factoryPromptDeferredInRam=true normalLauncher=false");
    }
    write_new("/newroot/etc/systemd/system/omarchy-wddm-normal-graphics.service",
        "[Unit]\nDescription=Owned normal-root NVIDIA integration experiment\nAfter=local-fs.target systemd-journald.service\n"
        "[Service]\nType=simple\nExecStartPre=/bin/sh -c '/usr/bin/getent passwd omarchy >/dev/null || /usr/bin/useradd -m -U -u 1000 -s /bin/bash omarchy'\n"
        "ExecStart=/opt/wddm-root/bin/normal-root-graphics-service\n"
        "ExecStopPost=/bin/sh -c 'echo NORMAL_ROOT_GRAPHICS_STOP serviceResult=$SERVICE_RESULT exitCode=$EXIT_CODE exitStatus=$EXIT_STATUS; /usr/bin/systemctl --no-block poweroff'\n"
        "StandardInput=null\nStandardOutput=tty\nStandardError=tty\nTTYPath=/dev/ttyS0\nTTYReset=no\nTTYVHangup=no\nRuntimeMaxSec=60\nTimeoutStopSec=10\n",0644);
    if(!access("/newroot/opt/wddm-root/desktop-coordinator-probe",F_OK)) {
        directory("/newroot/etc/systemd/system/omarchy-wddm-normal-graphics.service.d",0755);
        write_new("/newroot/etc/systemd/system/omarchy-wddm-normal-graphics.service.d/coordinator.conf",
            access("/newroot/opt/wddm-root/desktop-fence-probe",R_OK)==0 ? "[Service]\nRuntimeMaxSec=270\nTimeoutStopSec=5\n" : "[Service]\nRuntimeMaxSec=150\nTimeoutStopSec=5\n",0644);
        directory("/newroot/etc/systemd/system/user@1000.service.d",0755);
        write_new("/newroot/etc/systemd/system/user@1000.service.d/coordinator.conf","[Service]\nTimeoutStopSec=5\n",0644);
    }
    if(!access("/newroot/opt/wddm-root/desktop-shell-gpu-isolation",F_OK)) {
        directory("/newroot/etc/systemd/user",0755);
        if(symlink("/dev/null","/newroot/etc/systemd/user/xdg-desktop-portal.service") ||
           symlink("/dev/null","/newroot/etc/systemd/user/xdg-desktop-portal-hyprland.service")) fail("isolate-shell-gpu-clients");
        puts("NORMAL_ROOT_SHELL_GPU_ISOLATION xwayland=false portals=false softwareFallback=false ownedRamOverlayOnly=true");
    }
    if(symlink("../omarchy-wddm-normal-graphics.service","/newroot/etc/systemd/system/multi-user.target.wants/omarchy-wddm-normal-graphics.service")) fail("graphics-service-link");
    printf("NORMAL_ROOT_GRAPHICS_PAYLOAD copiedBytes=%llu ordinaryUid=1000 persistentWrites=false\n",(unsigned long long)copied_bytes);
}
#else
#include "normal_session_root_payload.h"
#endif
int main(void) {
    // Never allow this executable to perform mounts on a host or as a child.
    if(getpid()!=1 || getuid()!=0 || geteuid()!=0) return 125;
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    const int graphics=!access("/normal-root-overlay",F_OK);
#ifdef WDDM_NORMAL_SESSION_BOOTSTRAP
    if(!graphics || access("/normal-session-overlay",F_OK)) return 125;
#endif
    puts(graphics ? "NORMAL_ROOT_BOOT_BEGIN graphicsRequested=true sourceWritesAllowed=false" : "NORMAL_ROOT_BOOT_BEGIN graphics=false sourceWritesAllowed=false");
    directory("/dev",0755); directory("/proc",0555); directory("/sys",0555);
    if(mount("devtmpfs","/dev","devtmpfs",MS_NOSUID,"mode=0755") ||
       mount("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL) ||
       mount("sysfs","/sys","sysfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)) fail("early-mounts");
    for(unsigned tick=0;tick<400 && access("/dev/vda",F_OK);++tick) usleep(50000);
    const int ro=open("/sys/block/vda/ro",O_RDONLY|O_CLOEXEC);
    char state[3]={0};
    if(ro<0 || read(ro,state,sizeof(state))!=2 || memcmp(state,"1\n",2)) { errno=EROFS; fail("base-block-not-readonly"); }
    if(close(ro)) fail("base-ro-close");
    const int module=open("/overlay.ko",O_RDONLY|O_CLOEXEC);
    if(module<0 || syscall(SYS_finit_module,module,"",0)) fail("overlay-module");
    if(close(module)) fail("overlay-module-close");
    directory("/lower",0700); directory("/overlay",0700); directory("/newroot",0700);
    if(mount("/dev/vda","/lower","ext4",MS_RDONLY|MS_NOATIME,"noload")) fail("readonly-ext4");
    if(mount("tmpfs","/overlay","tmpfs",MS_NOSUID|MS_NODEV,"size=512m,mode=0700")) fail("bounded-overlay-tmpfs");
    directory("/overlay/upper",0700); directory("/overlay/work",0700);
    // The overlay upper becomes the OS root. Preserve the source root's
    // metadata so ordinary users and sandboxed system services can traverse it.
    struct stat root_metadata;
    if(stat("/lower",&root_metadata) || !S_ISDIR(root_metadata.st_mode) ||
       chown("/overlay/upper",root_metadata.st_uid,root_metadata.st_gid) ||
       chmod("/overlay/upper",root_metadata.st_mode&07777)) fail("overlay-root-metadata");
    printf("NORMAL_ROOT_METADATA_PRESERVED mode=%03o uid=%u gid=%u sourceWrites=false\n",
        (unsigned)(root_metadata.st_mode&07777),(unsigned)root_metadata.st_uid,(unsigned)root_metadata.st_gid);
    if(mount("overlay","/newroot","overlay",0,"lowerdir=/lower,upperdir=/overlay/upper,workdir=/overlay/work,index=off")) fail("temporary-root-overlay");
    // The source fstab describes a persistent ext4 root. This explicit boot
    // check uses the already-mounted RAM overlay and no persistent mounts.
    if(unlink("/newroot/etc/fstab") && errno!=ENOENT) fail("overlay-fstab-unlink");
    write_new("/newroot/etc/fstab","# Experimental read-only base with temporary RAM root; no persistent mounts.\n",0644);
    if(graphics) graphics_payload();
    else {
    directory("/newroot/usr/local/lib/try-omarchy",0755);
    write_new("/newroot/usr/local/lib/try-omarchy/wddm-normal-boot-check",
        "#!/bin/sh\nset -eu\n"
        "sleep 3\n"
        "test \"$(cat /sys/block/vda/ro)\" = 1\n"
        "test \"$(findmnt -n -o FSTYPE /)\" = overlay\n"
        "test -r /usr/share/omarchy/config/hypr/hyprland.lua\n"
        "printf 'NORMAL_ROOT_OS_VERSION '\ncat /usr/share/omarchy/version\n"
        "printf 'NORMAL_ROOT_SYSTEMD_VERSION '\nsystemctl --version | head -n 1\n"
        "printf 'NORMAL_ROOT_OS_READY sourceBlockReadonly=true rootOverlay=true normalSystemd=true graphics=false desktop=false\\n'\n"
        "systemctl --no-block poweroff\n",0755);
    directory("/newroot/etc/systemd/system",0755);
    directory("/newroot/etc/systemd/system/multi-user.target.wants",0755);
    write_new("/newroot/etc/systemd/system/omarchy-wddm-normal-boot.service",
        "[Unit]\nDescription=Read-only normal Omarchy boot prerequisite\nAfter=local-fs.target systemd-journald.service\n"
        "[Service]\nType=simple\nExecStart=/usr/local/lib/try-omarchy/wddm-normal-boot-check\n"
        "StandardOutput=journal+console\nStandardError=journal+console\nTimeoutStartSec=30\n",0644);
    if(symlink("../omarchy-wddm-normal-boot.service","/newroot/etc/systemd/system/multi-user.target.wants/omarchy-wddm-normal-boot.service")) fail("boot-check-service-link");
    }
    directory("/newroot/dev",0755); directory("/newroot/proc",0555); directory("/newroot/sys",0555);
    if(mount("/dev","/newroot/dev",NULL,MS_MOVE,NULL) || mount("/proc","/newroot/proc",NULL,MS_MOVE,NULL) ||
       mount("/sys","/newroot/sys",NULL,MS_MOVE,NULL)) fail("move-early-mounts");
    puts(graphics ? "NORMAL_ROOT_SWITCH_ROOT sourceBlockReadonly=true overlayBytes=536870912 graphicsRequested=true" : "NORMAL_ROOT_SWITCH_ROOT sourceBlockReadonly=true overlayBytes=536870912 graphics=false");
    if(chdir("/newroot") || mount(".","/",NULL,MS_MOVE,NULL) || chroot(".") || chdir("/")) fail("switch-root");
    char* const arguments[]={"/sbin/init",NULL};
    char* const environment[]={"PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin","TERM=linux",NULL};
    execve(arguments[0],arguments,environment); fail("exec-normal-systemd");
}
