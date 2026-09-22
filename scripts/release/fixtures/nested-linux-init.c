#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <unistd.h>
int main(void) {
 int fd=open("/dev/console",O_RDWR); if(fd>=0){dup2(fd,0);dup2(fd,1);dup2(fd,2);}
 struct utsname u; if(getpid()!=1 || uname(&u)) return 1;
 mkdir("/proc",0755); mount("proc","/proc","proc",0,0);
 char args[2048]={0}; fd=open("/proc/cmdline",O_RDONLY); if(fd>=0){read(fd,args,sizeof(args)-1);close(fd);}
 printf("TRY_OMARCHY_NESTED_LINUX_READY kernel=%s pid=%d\n",u.release,getpid());fflush(stdout);
 int mode=strstr(args,"tryomarchy.test=reboot") ? RB_AUTOBOOT : RB_POWER_OFF;
 printf("TRY_OMARCHY_NESTED_LINUX_EXIT %s\n",mode==RB_AUTOBOOT?"reboot":"poweroff");fflush(stdout);sync();
 reboot(mode);return 2;
}
