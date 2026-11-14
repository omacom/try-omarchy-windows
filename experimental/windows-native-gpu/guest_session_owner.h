// SPDX-License-Identifier: MIT
// Root-owned SDDM owner selection; read only, no account or login changes.
#pragma once
#include <pwd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
struct wddm_session_owner { uid_t uid;gid_t gid;char name[64]; };
static inline int wddm_session_read_root_file(const char* path,char* data,size_t capacity) {
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0) return -1;
    int result=-1;struct stat state;size_t used=0;
    if(fstat(fd,&state) || !S_ISREG(state.st_mode) || state.st_uid || (state.st_mode&0022) ||
       state.st_nlink!=1 || state.st_size<=0 || (uint64_t)state.st_size>=capacity) goto done;
    for(;;) {
        if(used>=capacity-1) goto done;
        const ssize_t count=read(fd,data+used,capacity-1-used);
        if(count<0 && errno==EINTR) continue;
        if(count<0) goto done;
        if(!count) break;
        used+=(size_t)count;
    }
    if(used!=(size_t)state.st_size || memchr(data,0,used)) goto done;
    data[used]=0;result=0;
done:
    if(close(fd)) result=-1;
    return result;
}
static inline char* wddm_session_trim(char* text) {
    while(*text==' ' || *text=='\t' || *text=='\r') ++text;
    char* end=text+strlen(text);
    while(end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r')) *--end=0;
    return text;
}
static inline int wddm_session_owner_name(char* config,char name[64]) {
    unsigned sections=0,user=0,session=0;int active=0;
    for(char* line=config;line && *line;) {
        char* next=strchr(line,'\n');if(next) *next++=0;
        char* item=wddm_session_trim(line);
        if(!*item || *item=='#' || *item==';') { line=next;continue; }
        if(*item=='[') {
            active=!strcmp(item,"[Autologin]");
            if(active && ++sections!=1) return -1;
        } else if(active) {
            char* equals=strchr(item,'=');if(!equals) return -1;
            *equals++=0;char* key=wddm_session_trim(item);char* value=wddm_session_trim(equals);
            if(!strcmp(key,"User")) {
                if(user++ || !*value || strlen(value)>=64 || strspn(value,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=strlen(value)) return -1;
                memcpy(name,value,strlen(value)+1);
            } else if(!strcmp(key,"Session")) {
                if(session++ || strcmp(value,"omarchy.desktop")) return -1;
            }
        }
        line=next;
    }
    return sections==1 && user==1 && session==1?0:-1;
}
static inline int wddm_session_owner_load(const char* path,struct wddm_session_owner* owner) {
    char data[4096];memset(owner,0,sizeof *owner);
    if(wddm_session_read_root_file(path,data,sizeof data) || wddm_session_owner_name(data,owner->name)) return -1;
    const struct passwd* user=getpwnam(owner->name);
    if(!user || !user->pw_uid || !user->pw_gid || user->pw_uid==(uid_t)-1 || user->pw_gid==(gid_t)-1) return -1;
    owner->uid=user->pw_uid;owner->gid=user->pw_gid;return 0;
}
