// SPDX-License-Identifier: MIT
// Private, disk-free Linux runtime diagnostic. No guest disk is attached.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "guest-kernel/wddm_aperture.h"
#include "guest-kernel/wddm_render.h"
#include <dirent.h>
#include "guest_primary_identity.h"
static void timeout_handler(int signal_number) { (void)signal_number; }
static int graphics_primary_writer=-1;
static inline int graphics_primary_register(pid_t child,int process) {
    if(graphics_primary_writer<0) return -1;
    const int registered=wddm_primary_send(graphics_primary_writer,child,process);
    close(graphics_primary_writer);graphics_primary_writer=-1;
    return registered;
}
static pid_t graphics_broker_start_as(const char* port,unsigned uid,unsigned gid) {
    if (!uid || !gid) return -1;
    if (access("/graphics-broker-required", F_OK)) return 0;
    int ready[2];
    if ((mkdir("/run",0755) && errno != EEXIST) || mkdir("/run/wddm-broker",0755) || pipe2(ready,O_CLOEXEC)) return -1;
    const int coordinated=access("/opt/wddm-root/desktop-coordinator-probe",R_OK)==0;
    int primary[2]={-1,-1};
    if(coordinated && socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,primary)) { close(ready[0]);close(ready[1]);return -1; }
    const pid_t child=fork();
    if (!child) {
        close(ready[0]);
        if (fcntl(ready[1],F_SETFD,0)) _exit(127);
        char descriptor[24]; snprintf(descriptor,sizeof descriptor,"%d",ready[1]);
        char user[16],group[16]; snprintf(user,sizeof user,"%u",uid); snprintf(group,sizeof group,"%u",gid);
        unsetenv("LD_PRELOAD"); setenv("LD_LIBRARY_PATH","/usr/lib/x86_64-linux-gnu",1);
        if(coordinated) {
            if(!access("/opt/wddm-root/desktop-reference-probe",R_OK) && setenv("WDDM_BRIDGE_COORDINATED_REFERENCES","1",1)) _exit(127);
            if(!access("/opt/wddm-root/desktop-retirement-probe",R_OK) && setenv("WDDM_BRIDGE_COORDINATED_RETIREMENT","1",1)) _exit(127);
            close(primary[1]);if(fcntl(primary[0],F_SETFD,0)) _exit(127);
            char identity[24];snprintf(identity,sizeof identity,"%d",primary[0]);
            execl("/guest-graphics-broker","/guest-graphics-broker","/run/wddm-broker/graphics.sock",port,user,group,descriptor,"--coordinator",identity,(char*)NULL);
            _exit(127);
        }
        execl("/guest-graphics-broker","/guest-graphics-broker","/run/wddm-broker/graphics.sock",port,user,group,descriptor,(char*)NULL);
        _exit(127);
    }
    if(coordinated) { close(primary[0]);graphics_primary_writer=primary[1]; }
    close(ready[1]);
    struct sigaction action={0}; action.sa_handler=timeout_handler; sigaction(SIGALRM,&action,NULL);
    alarm(10); char marker=0; const ssize_t bytes=read(ready[0],&marker,1); alarm(0); close(ready[0]);
    if (child < 0 || bytes != 1 || marker != 'R') {
        if(graphics_primary_writer>=0) { close(graphics_primary_writer);graphics_primary_writer=-1; }
        if (child > 0) { kill(child,SIGKILL); waitpid(child,NULL,0); }
        puts("BROKER_ROUTE failed=true readiness=true"); return -1;
    }
    return child;
}
static pid_t graphics_broker_start(const char* port) { return graphics_broker_start_as(port,65534,65534); }
static int graphics_client_drop_as(const char* port,unsigned uid,unsigned gid) {
    if (!uid || !gid) return -1;
    // The ordinary process must never inherit the trusted primary-PID writer.
    if(graphics_primary_writer>=0) { close(graphics_primary_writer);graphics_primary_writer=-1; }
    unsetenv("WDDM_BRIDGE_PORT"); unsetenv("WDDM_BRIDGE_WINDOWS_WORKER");
    setenv("WDDM_BRIDGE_BROKER_SOCKET","/run/wddm-broker/graphics.sock",1);
    if (setgroups(0,NULL) || setgid(gid) || setuid(uid) || prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)) return -1;
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct capabilities[2]={{0}};
    if (syscall(SYS_capget,&header,capabilities) || capabilities[0].effective || capabilities[1].effective ||
        getuid()!=uid || geteuid()!=uid || getgid()!=gid || getegid()!=gid || getgroups(0,NULL) ||
        prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1) return -1;
    const char* paths[]={"/dev/wddm-aperture",port};
    for (unsigned i=0;i<2;++i) {
        const int fd=open(paths[i],O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (fd>=0) { close(fd); return -1; }
        if (errno!=EACCES && errno!=EPERM) return -1;
    }
    printf("BROKER_RENDER_CLIENT uid=%u gid=%u effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied\n",uid,gid);
    fflush(stdout); return 0;
}
static int graphics_client_drop(const char* port) { return graphics_client_drop_as(port,65534,65534); }
static int graphics_broker_wait(pid_t child) {
    if(graphics_primary_writer>=0) { close(graphics_primary_writer);graphics_primary_writer=-1; }
    if (child<=0) return child<0 ? -1 : 0;
    for (unsigned n=0;n<500;++n) {
        int status=0; const pid_t result=waitpid(child,&status,WNOHANG);
        if (result==child) {
            const int success=WIFEXITED(status) && !WEXITSTATUS(status);
            printf("BROKER_ROUTE_REAP exit=%d\n",success ? 0 : 1); return success ? 0 : -1;
        }
        if (result<0 && errno!=EINTR) break;
        usleep(10000);
    }
    // The rendering child has already exited or been killed and reaped.
    kill(child,SIGKILL); waitpid(child,NULL,0); puts("BROKER_ROUTE failed=true reapTimeout=true"); return -1;
}
static int broker_peer_start(void) {
    if (access("/broker-peer-required", F_OK)) return 0;
    pid_t child = fork();
    if (!child) {
        signal(SIGALRM, SIG_DFL); alarm(30);
        unsetenv("LD_PRELOAD");
        setenv("LD_LIBRARY_PATH", "/usr/lib/x86_64-linux-gnu", 1);
        execl("/broker-peer-contract", "/broker-peer-contract", "/broker-finalizer-probe.so", (char*)NULL);
        _exit(127);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        puts("BROKER_PEER_START failed=true"); return -1;
    }
    puts("BROKER_PEER_START contracts=true gpuRouting=false");
    return 0;
}
static int kernel_aperture_start(void) {
    if (access("/kernel-aperture-required", F_OK)) return 0;
    const int module = open("/wddm-aperture.ko", O_RDONLY | O_CLOEXEC);
    if (module < 0) { perror("aperture module open"); return -1; }
    char parameters[256]={0};
    const struct { const char* marker;const char* value; } options[]={
        {"/native-sync-files-required","enable_native_sync_files=1 "},
        {"/native-buffers-required","enable_native_buffers=1 "},
        {"/opt/wddm-root/desktop-4k-cpu-probe","enable_desktop_4k_cpu=1 "},
        {"/opt/wddm-root/desktop-sparse-probe","enable_sparse_aperture=1 "},
        {"/opt/wddm-root/desktop-coordinator-probe","enable_coordinated_leases=1 "},
        {"/opt/wddm-root/desktop-fence-probe","enable_desktop_fences=1 "},
        {"/opt/wddm-root/desktop-slot-probe","enable_desktop_slots=1 "},
        {"/opt/wddm-root/desktop-lease-probe","enable_desktop_leases=1 "},
        {"/opt/wddm-root/multiapp-cpu-probe","enable_multiapp_cpu=1 "},
    };
    for(size_t index=0;index<sizeof options/sizeof options[0];++index) {
        if(access(options[index].marker,F_OK)) continue;
        const size_t used=strlen(parameters),length=strlen(options[index].value);
        if(length>=sizeof parameters-used) { close(module);errno=EOVERFLOW;perror("aperture module parameters");return -1; }
        memcpy(parameters+used,options[index].value,length+1);
    }
    const int result = (int)syscall(SYS_finit_module, module, parameters, 0);
    close(module);
    if (result) { perror("guest aperture finit_module"); return -1; }
    pid_t child = fork();
    if (!child) { execl("/aperture-contract", "/aperture-contract", (char*)NULL); _exit(127); }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        puts("KERNEL_APERTURE_START failed=true"); return -1;
    }
    puts("KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false");
    return 1;
}
static char native_render_path[64];
static int native_render_loaded;
static int native_render_start(void) {
    if (access("/native-render-required", F_OK)) return 0;
    const int module = open("/wddm-render.ko", O_RDONLY | O_CLOEXEC);
    if (module < 0) { perror("render module open"); return -1; }
    errno = 0;
    const int disabled = (int)syscall(SYS_finit_module, module, "", 0);
    const int disabled_error = errno;
    if (!disabled || disabled_error != EOPNOTSUPP) {
        native_render_loaded = !disabled;
        close(module); puts("KERNEL_RENDER_START defaultDisabled=false"); return -1;
    }
    const int prime = !access("/opt/wddm-root/desktop-coordinator-probe", R_OK);
    const int loaded = (int)syscall(SYS_finit_module, module, prime ? "experimental=1 enable_native_prime=1" : "experimental=1", 0);
    close(module);
    if (loaded) { perror("render finit_module"); return -1; }
    native_render_loaded = 1;
    unsigned matches = 0;
    for (unsigned n = 128; n < 256; ++n) {
        char path[64]; snprintf(path, sizeof(path), "/dev/dri/renderD%u", n);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        struct wddm_render_info info = {.version = WDDM_RENDER_VERSION};
        if (fd < 0) {
            if(errno!=ENOENT) printf("KERNEL_RENDER_NODE_OPEN node=%s errno=%d\n",path,errno);
            continue;
        }
        errno=0; const int queried=ioctl(fd, DRM_IOCTL_WDDM_RENDER_INFO, &info);
        printf("KERNEL_RENDER_NODE_QUERY node=%s result=%d errno=%d version=%u flags=%u signature=%016llx width=%u height=%u\n",
            path,queried,errno,info.version,info.flags,(unsigned long long)info.signature,info.max_width,info.max_height);
        if (!queried && info.version == WDDM_RENDER_VERSION &&
            info.flags == WDDM_RENDER_NATIVE_ROUTE && info.signature == WDDM_RENDER_MAGIC &&
            info.max_width == 4096 && info.max_height == 4096 && !info.reserved[0] && !info.reserved[1]) {
            ++matches; memcpy(native_render_path, path, strlen(path)+1);
        }
        close(fd);
    }
    if (matches != 1 || chmod(native_render_path, 0666)) {
        printf("KERNEL_RENDER_START identity=false matches=%u errno=%d\n",matches,errno);
        DIR* nodes=opendir("/sys/class/drm");
        if(nodes) {
            struct dirent* node;
            while((node=readdir(nodes))) printf("KERNEL_RENDER_SYSFS name=%s\n",node->d_name);
            closedir(nodes);
        }
        return -1;
    }
    if (prime) {
        pid_t probe=fork();
        if (!probe) { execl("/opt/wddm-root/bin/native-prime-contract","native-prime-contract",native_render_path,(char*)NULL);_exit(127); }
        int status=0;
        if (probe<0 || waitpid(probe,&status,0)!=probe || !WIFEXITED(status) || WEXITSTATUS(status)) return -1;
        printf("KERNEL_RENDER_NATIVE_PRIME verified=true opaqueOwnedImports=true gpu=false\n");
    }
    printf("KERNEL_RENDER_START node=%s defaultDisabled=true nativeRoute=true kms=false gem=%s\n", native_render_path,prime ? "opaque-import" : "false");
    return 1;
}
static int native_render_stop(void) {
    if (syscall(SYS_delete_module, "wddm_render", O_NONBLOCK)) {
        perror("render delete_module"); return -1;
    }
    native_render_loaded = 0;
    if (native_render_path[0] && !access(native_render_path, F_OK)) {
        puts("KERNEL_RENDER_EXIT nodeRemoved=false"); return -1;
    }
    puts("KERNEL_RENDER_EXIT unloaded=true nodeRemoved=true");
    return 0;
}
static int kernel_aperture_stop(void) {
    const int fd = open("/dev/wddm-aperture", O_RDWR | O_CLOEXEC);
    struct wddm_aperture_info info = {.version=WDDM_APERTURE_VERSION};
    const int result = fd < 0 ? -1 : ioctl(fd, WDDM_APERTURE_INFO, &info);
    struct wddm_object_stats objects = {.version=WDDM_APERTURE_VERSION};
    const int object_result = fd < 0 ? -1 : ioctl(fd, WDDM_OBJECT_STATS, &objects);
    const int object_error = errno;
    struct wddm_sync_file_stats syncs={.version=1};
    int sync_result=fd<0 ? -1:ioctl(fd,WDDM_SYNC_FILE_STATS,&syncs);
    int sync_error=errno;
    for(unsigned n=0;!sync_result && (syncs.held || syncs.pending) && n<2000;++n) {
        usleep(1000); syncs.version=1; sync_result=ioctl(fd,WDDM_SYNC_FILE_STATS,&syncs); sync_error=errno;
    }
    const int native_buffers=!access("/native-buffers-required",F_OK);
    struct wddm_native_buffer_stats buffers={.version=1};
    int buffer_result=native_buffers ? (fd<0 ? -1 : ioctl(fd,WDDM_NATIVE_BUFFER_STATS,&buffers)) : 0;
    for(unsigned n=0;native_buffers && !buffer_result && buffers.held && n<2000;++n) {
        usleep(1000); buffers.version=1; buffer_result=ioctl(fd,WDDM_NATIVE_BUFFER_STATS,&buffers);
    }
    struct wddm_fence_eventfd_stats events={.version=WDDM_APERTURE_VERSION};
    int event_result=fd<0 ? -1 : ioctl(fd,WDDM_FENCE_EVENTFD_STATS,&events);
    int event_error=errno;
    for(unsigned n=0;!event_result && (events.held || events.pending) && n<2000;++n) {
        usleep(1000); events=(struct wddm_fence_eventfd_stats){.version=WDDM_APERTURE_VERSION};
        event_result=ioctl(fd,WDDM_FENCE_EVENTFD_STATS,&events); event_error=errno;
    }
    if (fd >= 0) close(fd);
    if(!event_result) {
        if(events.held || events.pending || events.created!=events.released ||
           events.notified+events.errors!=events.created) {
            puts("KERNEL_EVENTFD_EXIT cleanup=false"); return -1;
        }
        printf("KERNEL_EVENTFD_EXIT held=0 pending=0 created=%llu notified=%llu errors=%llu released=%llu cleanup=true\n",
            (unsigned long long)events.created,(unsigned long long)events.notified,
            (unsigned long long)events.errors,(unsigned long long)events.released);
    } else if(event_error!=ENOTTY && event_error!=EOPNOTSUPP) {
        puts("KERNEL_EVENTFD_EXIT cleanup=false"); return -1;
    }
    if(native_buffers) {
        if(buffer_result || buffers.held || buffers.bytes || buffers.created!=buffers.released) {
            puts("KERNEL_NATIVE_BUFFER_EXIT cleanup=false"); return -1;
        }
        printf("KERNEL_NATIVE_BUFFER_EXIT held=0 bytes=0 created=%llu released=%llu cleanup=true\n",
            (unsigned long long)buffers.created,(unsigned long long)buffers.released);
    }
    if (result || info.active_leases || info.active_cpu_bytes || info.fence_mmio_reads ||
        info.allocation_mmio_reads || info.allocation_mmio_writes) {
        puts("KERNEL_APERTURE_EXIT cleanup=false"); return -1;
    }
    if ((!object_result && (objects.held || objects.active)) || (object_result && object_error != ENOTTY)) {
        puts("KERNEL_OBJECT_EXIT cleanup=false"); return -1;
    }
    if (!object_result) puts("KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true");
    if ((!sync_result && (syncs.held || syncs.pending)) || (sync_result && sync_error!=ENOTTY)) {
        puts("KERNEL_SYNC_FILE_EXIT cleanup=false"); return -1;
    }
    if (!sync_result) printf("KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=%llu signaled=%llu errors=%llu released=%llu cleanup=true\n",
        (unsigned long long)syncs.created,(unsigned long long)syncs.signaled,(unsigned long long)syncs.errors,(unsigned long long)syncs.released);
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        if (!syscall(SYS_delete_module, "wddm_aperture", O_NONBLOCK)) {
            puts("KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true");
            return 0;
        }
        if (errno != EWOULDBLOCK && errno != EBUSY) break;
        usleep(50000);
    }
    perror("guest aperture delete_module"); return -1;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    struct rlimit core = {0, 0}; setrlimit(RLIMIT_CORE, &core);
    mkdir("/dev", 0755); mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/tmp", 01777);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) && errno != EBUSY) perror("devtmpfs");
    mount("proc", "/proc", "proc", 0, NULL); mount("sysfs", "/sys", "sysfs", 0, NULL);
    mkdir("/dev/shm", 01777); mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");
    char command[512] = {0};
    FILE* cmdline = fopen("/proc/cmdline", "rb");
    if (cmdline) { const size_t bytes = fread(command, 1, sizeof(command) - 1, cmdline); command[bytes] = 0; fclose(cmdline); }
    const char* store_option = strstr(command, " wddm_cpu_store_test=1");
    const size_t option_bytes = sizeof(" wddm_cpu_store_test=1") - 1;
    const int cpu_store_test = store_option && (store_option[option_bytes] == 0 || store_option[option_bytes] == ' ' || store_option[option_bytes] == '\n');
    const char* eof_option = strstr(command, " wddm_cpu_eof_test=1");
    const size_t eof_bytes = sizeof(" wddm_cpu_eof_test=1") - 1;
    const int cpu_eof_test = eof_option && (eof_option[eof_bytes] == 0 || eof_option[eof_bytes] == ' ' || eof_option[eof_bytes] == '\n');
    const char* span_eof_option = strstr(command, " wddm_cpu_span_eof_test=1");
    const size_t span_eof_bytes = sizeof(" wddm_cpu_span_eof_test=1") - 1;
    const int cpu_span_eof_test = span_eof_option && (span_eof_option[span_eof_bytes] == 0 || span_eof_option[span_eof_bytes] == ' ' || span_eof_option[span_eof_bytes] == '\n');
    const char* sync_eof_option = strstr(command, " wddm_sync_eof_test=1");
    const size_t sync_eof_bytes = sizeof(" wddm_sync_eof_test=1") - 1;
    const int sync_eof_test = sync_eof_option && (sync_eof_option[sync_eof_bytes] == 0 ||
        sync_eof_option[sync_eof_bytes] == ' ' || sync_eof_option[sync_eof_bytes] == '\n');
    const char* sync_no_max_eof_option = strstr(command, " wddm_sync_no_max_eof_test=1");
    const size_t sync_no_max_eof_bytes = sizeof(" wddm_sync_no_max_eof_test=1") - 1;
    const int sync_no_max_eof_test = sync_no_max_eof_option && (sync_no_max_eof_option[sync_no_max_eof_bytes] == 0 ||
        sync_no_max_eof_option[sync_no_max_eof_bytes] == ' ' || sync_no_max_eof_option[sync_no_max_eof_bytes] == '\n');
    const char* reservation_eof_option = strstr(command, " wddm_reservation_eof_test=1");
    const size_t reservation_eof_bytes = sizeof(" wddm_reservation_eof_test=1") - 1;
    const int reservation_eof_test = reservation_eof_option && (reservation_eof_option[reservation_eof_bytes] == 0 ||
        reservation_eof_option[reservation_eof_bytes] == ' ' || reservation_eof_option[reservation_eof_bytes] == '\n');
    const char* gpu_state_eof_option = strstr(command, " wddm_gpu_state_eof_test=1");
    const size_t gpu_state_eof_bytes = sizeof(" wddm_gpu_state_eof_test=1") - 1;
    const int gpu_state_eof_test = gpu_state_eof_option && (gpu_state_eof_option[gpu_state_eof_bytes] == 0 ||
        gpu_state_eof_option[gpu_state_eof_bytes] == ' ' || gpu_state_eof_option[gpu_state_eof_bytes] == '\n');
    const char* hwqueue_eof_option = strstr(command, " wddm_hwqueue_eof_test=1");
    const size_t hwqueue_eof_bytes = sizeof(" wddm_hwqueue_eof_test=1") - 1;
    const int hwqueue_eof_test = hwqueue_eof_option && (hwqueue_eof_option[hwqueue_eof_bytes] == 0 ||
        hwqueue_eof_option[hwqueue_eof_bytes] == ' ' || hwqueue_eof_option[hwqueue_eof_bytes] == '\n');
    const char* hwqueue_no_broadcast_option = strstr(command, " wddm_hwqueue_no_broadcast_eof_test=1");
    const size_t hwqueue_no_broadcast_bytes = sizeof(" wddm_hwqueue_no_broadcast_eof_test=1") - 1;
    const int hwqueue_no_broadcast_eof_test = hwqueue_no_broadcast_option &&
        (hwqueue_no_broadcast_option[hwqueue_no_broadcast_bytes] == 0 || hwqueue_no_broadcast_option[hwqueue_no_broadcast_bytes] == ' ' ||
         hwqueue_no_broadcast_option[hwqueue_no_broadcast_bytes] == '\n');
    const char* port = "/dev/vport0p1";
    const char* hwqueue_wait_option = strstr(command, " wddm_hwqueue_no_broadcast_wait_eof_test=1");
    const size_t hwqueue_wait_bytes = sizeof(" wddm_hwqueue_no_broadcast_wait_eof_test=1") - 1;
    const int hwqueue_wait_eof_test = hwqueue_wait_option &&
        (hwqueue_wait_option[hwqueue_wait_bytes] == 0 || hwqueue_wait_option[hwqueue_wait_bytes] == ' ' || hwqueue_wait_option[hwqueue_wait_bytes] == '\n');
    for (int i = 0; i < 200 && access(port, F_OK); ++i) usleep(50000);
    const int kernel_aperture = broker_peer_start() ? -1 : kernel_aperture_start();
    const int kernel_render = kernel_aperture >= 0 ? native_render_start() : -1;
    const pid_t graphics_broker=kernel_aperture>0 && kernel_render>=0 ? graphics_broker_start(port) :
        (access("/graphics-broker-required",F_OK) ? 0 : -1);
    char module[130] = {0};
    FILE* name = fopen("/linux-umd-name", "rb");
    int status = 127;
    unsigned runtime_deadline = 40;
    FILE* deadline_workload = fopen("/runtime-workload", "rb");
    if (deadline_workload) {
        char value[14] = {0}; const size_t bytes = fread(value, 1, sizeof value, deadline_workload);
        if (bytes == 13 && !memcmp(value, "native-stream", 13)) runtime_deadline = 120;
        fclose(deadline_workload);
    }
    if (kernel_aperture >= 0 && kernel_render>=0 && graphics_broker>=0 && name && fread(module, 1, sizeof(module) - 1, name) && !access(port, F_OK)) {
        module[strcspn(module, "\r\n")] = 0;
        pid_t child = fork();
        if (child == 0) {
            FILE* workload_file = fopen("/runtime-workload", "rb");
            if (workload_file) {
                char workload[14] = {0}; const size_t count = fread(workload, 1, sizeof(workload), workload_file);
                fclose(workload_file);
                if (count == 4 && !memcmp(workload, "copy", 4)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "copy", 1);
                else if (count == 5 && !memcmp(workload, "clear", 5)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "clear", 1);
                else if (count == 8 && !memcmp(workload, "triangle", 8)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "triangle", 1);
                else if (count == 6 && !memcmp(workload, "shared", 6)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "shared", 1);
                else if (count == 13 && !memcmp(workload, "native-stream", 13)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "native-stream", 1);
                else if (count == 6 && !memcmp(workload, "native", 6)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "native", 1);
                else if (count == 7 && !memcmp(workload, "consume", 7)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "consume", 1);
                else if (count == 6 && !memcmp(workload, "opengl", 6)) {
                    setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "opengl", 1);
                    setenv("EGL_PLATFORM", "surfaceless", 1);
                    setenv("GALLIUM_DRIVER", "d3d12", 1);
                    setenv("MESA_D3D12_DEFAULT_ADAPTER_NAME", "NVIDIA", 1);
                    setenv("LIBGL_DRIVERS_PATH", "/opt/wddm-mesa/lib/dri", 1);
                    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
                    setenv("EGL_LOG_LEVEL", "debug", 1);
                }
                else if (count != 4 || memcmp(workload, "init", 4)) _exit(126);
            }
            setenv("WDDM_BRIDGE_PORT", port, 1);
            if (kernel_aperture) setenv("WDDM_BRIDGE_KERNEL_APERTURE", "1", 1);
            if (cpu_store_test) {
                setenv("WDDM_BRIDGE_CPU_STORE_TEST", "1", 1);
                setenv("WDDM_BRIDGE_CPU_REFERENCE_TEST", "1", 1);
            }
            if (cpu_eof_test) setenv("WDDM_BRIDGE_CPU_EOF_TEST", "1", 1);
            if (cpu_span_eof_test) setenv("WDDM_BRIDGE_CPU_SPAN_EOF_TEST", "1", 1);
            if (sync_eof_test) setenv("WDDM_BRIDGE_SYNC_EOF_TEST", "1", 1);
            if (sync_no_max_eof_test) setenv("WDDM_BRIDGE_SYNC_NO_MAX_EOF_TEST", "1", 1);
            if (reservation_eof_test) setenv("WDDM_BRIDGE_RESERVATION_EOF_TEST", "1", 1);
            if (gpu_state_eof_test) setenv("WDDM_BRIDGE_GPU_STATE_EOF_TEST", "1", 1);
            if (hwqueue_eof_test) setenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST", "1", 1);
            if (hwqueue_no_broadcast_eof_test) setenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST", "2", 1);
            if (hwqueue_wait_eof_test) setenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST", "3", 1);
            setenv("WDDM_BRIDGE_LINUX_UMD_NAME", module, 1);
            if (kernel_render>0) setenv("WDDM_BRIDGE_RENDER_NODE", native_render_path, 1);
            setenv("LD_PRELOAD", "/linux-ioctl-bridge.so", 1);
            const char* selected_workload = getenv("WDDM_BRIDGE_RUNTIME_WORKLOAD");
            setenv("LD_LIBRARY_PATH", selected_workload && !strcmp(selected_workload, "opengl") ?
                "/opt/wddm-mesa/lib:/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu" :
                "/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu", 1);
            if (graphics_broker>0 && graphics_client_drop(port)) { perror("drop broker rendering client privileges"); _exit(126); }
            if (!access("/runtime-tracer", X_OK)) {
                unsetenv("LD_PRELOAD");
                execl("/runtime-tracer", "/runtime-tracer", "-f", "-e", "status=failed", "-e",
                      "trace=memory,file,eventfd2,memfd_create,prlimit64", "-o", "/dev/console",
                      "-E", "LD_PRELOAD=/linux-ioctl-bridge.so", "/guest-probe", (char*)NULL);
                perror("exec tracer"); _exit(127);
            }
            execl("/guest-probe", "/guest-probe", (char*)NULL); perror("exec runtime probe"); _exit(127);
        }
        if (child > 0) {
            struct sigaction action = {0}; action.sa_handler = timeout_handler;
            sigaction(SIGALRM, &action, NULL); alarm(runtime_deadline);
            if (waitpid(child, &status, 0) == child) {
                if (WIFSIGNALED(status))
                    printf("BRIDGE_RUNTIME_SIGNAL number=%d coreDumped=%s\n", WTERMSIG(status), WCOREDUMP(status) ? "true" : "false");
                status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            }
            else { kill(child, SIGKILL); waitpid(child, NULL, 0); status = 124; }
            alarm(0);
        }
    } else puts(kernel_aperture < 0 ? "FAIL: required guest aperture contracts failed" : "FAIL: runtime name or virtio port missing");
    if (name) fclose(name);
    if (graphics_broker_wait(graphics_broker)) status=125;
    if (native_render_loaded && native_render_stop()) status=125;
    if (kernel_aperture > 0 && kernel_aperture_stop()) status = 125;
    printf("BRIDGE_RUNTIME_EXIT=%d\n", status);
    sync(); reboot(RB_POWER_OFF);
    for (;;) pause();
}
