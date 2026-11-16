// SPDX-License-Identifier: MIT
// Exercise receipt boundaries with another process sharing the same console.
#define WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT 1
#define WDDM_BRIDGE_COMPOSITOR_HOST_SCENE 1
#define __libc_start_main unused_compositor_interposer_entry
#include "hyprland_startup_probe.c"
#undef __libc_start_main

static int prefix_boundary_control(const char* receipt) {
    const char* partial="unfinished unrelated trace";
    const char* hex_line="\nSCENE_TEST_HEX 00ff\n";
    int channel[2];if(pipe(channel)) return 1;
    const pid_t child=fork();if(child<0) return 1;
    if(!child) {
        close(channel[0]);
        if(dup2(channel[1],STDOUT_FILENO)<0) _exit(1);
        close(channel[1]);
        if(write(STDOUT_FILENO,partial,strlen(partial))!=(ssize_t)strlen(partial) || scene_receipt(receipt)) _exit(2);
        scene_hex_receipt("SCENE_TEST_HEX","\0\xff",2);
        _exit(atomic_load(&queue_failed) ? 3 : 0);
    }
    close(channel[1]);char actual[256],expected[256];size_t used=0;
    for(;;) {
        const ssize_t count=read(channel[0],actual+used,sizeof(actual)-used);
        if(count<0 && errno==EINTR) continue;
        if(count<0 || (count>0 && used+(size_t)count==sizeof(actual))) return 1;
        if(!count) break;
        used+=(size_t)count;
    }
    close(channel[0]);int status=0;
    if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    const int bytes=snprintf(expected,sizeof(expected),"%s\n%s%s",partial,receipt,hex_line);
    return bytes<0 || (size_t)bytes!=used || memcmp(actual,expected,used);
}

int main(void) {
    pid_t terminal_pid=0;
    if(!visible_gpu_terminal_with_pid("[{\"workspace\":{\"id\":1},\"pid\":123,\"class\":\"omarchy-gpu-terminal\",\"mapped\":true,\"hidden\":false}]",&terminal_pid) || terminal_pid!=123 ||
       visible_gpu_terminal_with_pid("[{\"pid\":321,\"class\":\"other\"},{\"class\":\"omarchy-gpu-terminal\",\"mapped\":true,\"hidden\":false}]",&terminal_pid) ||
       visible_gpu_terminal_with_pid("{\"pid\":-1,\"class\":\"omarchy-gpu-terminal\",\"mapped\":true,\"hidden\":false}",&terminal_pid)) return 1;
    if(!visible_gpu_terminal("{\"mapped\":true,\"hidden\":false,\"class\":\"omarchy-gpu-terminal\"}") ||
       !visible_gpu_terminal("[{\"mapped\":true,\"hidden\":false,\"workspace\":{\"id\":1},\"title\":\"{terminal} \\\"quoted\\\"\",\"class\":\"omarchy-gpu-terminal\"}]") ||
       visible_gpu_terminal("{\"mapped\":false,\"hidden\":false,\"class\":\"omarchy-gpu-terminal\"}") ||
       visible_gpu_terminal("{\"mapped\":true,\"hidden\":true,\"class\":\"omarchy-gpu-terminal\"}") ||
       visible_gpu_terminal("{\"mapped\":true,\"hidden\":false,\"class\":\"another-app\"}")) return 1;
    if(visible_shell_bar("{\"namespace\":\"omarchy-bar\",\"alpha\":0}") ||
       visible_shell_bar("{\"alpha\":0.5,\"namespace\":\"omarchy-bar\"}") ||
       visible_shell_bar("{\"namespace\":\"omarchy-bar\"}") ||
       visible_shell_bar("{\"namespace\":\"omarchy-bar\",\"alpha\":\"1\"}") ||
       !visible_shell_bar("{\"alpha\":1,\"namespace\":\"omarchy-bar\"}") ||
       !visible_shell_bar("{\"namespace\":\"omarchy-bar\",\"alpha\":0.999}")) return 1;
    const char* receipt="HYPRLAND_SCENE_CLOSED clientUnmapped=true childExitMarker=true\n";
    const char* other="LINUX_BRIDGE competing-process-log\n";
    if(prefix_boundary_control(receipt)) return 1;
    int channel[2];
    if(pipe(channel)) return 1;
    pid_t children[2];
    for(unsigned writer=0;writer<2;++writer) {
        children[writer]=fork();
        if(children[writer]<0) return 1;
        if(!children[writer]) {
            close(channel[0]);
            if(dup2(channel[1],STDOUT_FILENO)<0) _exit(1);
            close(channel[1]);
            for(unsigned line=0;line<1000;++line) {
                if(writer ? write(STDOUT_FILENO,other,strlen(other))!=(ssize_t)strlen(other) : scene_receipt(receipt)) _exit(2);
            }
            // A failed console write must invalidate acceptance.
            if(!writer && (close(STDOUT_FILENO) || !scene_receipt(receipt) || !atomic_load(&queue_failed))) _exit(3);
            _exit(0);
        }
    }
    close(channel[1]);
    FILE* stream=fdopen(channel[0],"r");
    if(!stream) return 1;
    char line[128];unsigned receipts=0,competing=0,boundaries=0;
    while(fgets(line,sizeof line,stream)) {
        if(!strcmp(line,receipt)) ++receipts;
        else if(!strcmp(line,other)) ++competing;
        else if(!strcmp(line,"\n")) ++boundaries;
        else return 1;
    }
    if(ferror(stream) || fclose(stream) || receipts!=1000 || competing!=1000 || boundaries!=1000) return 1;
    for(unsigned writer=0;writer<2;++writer) {
        int status=0;
        if(waitpid(children[writer],&status,0)!=children[writer] || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    }
    puts("PASS: exact leading boundary after unfinished trace, hex framing and 2000 concurrent console records; failed receipt rejects acceptance; gpu=false");
    return 0;
}
