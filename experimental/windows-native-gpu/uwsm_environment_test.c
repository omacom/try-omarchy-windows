// SPDX-License-Identifier: MIT
#define main wddm_unused_compositor_exec_main
#include "uwsm_compositor_exec.c"
#undef main
#include <assert.h>
int main(void) {
    char valid[]="WDDM_TEST_LITERAL=\"$(false); no evaluation\"\nWDDM_TEST_EMPTY=\"\"\n";
    assert(!compositor_environment(valid,sizeof valid-1));
    assert(!strcmp(getenv("WDDM_TEST_LITERAL"),"$(false); no evaluation") && !*getenv("WDDM_TEST_EMPTY"));
    const char* invalid[]={"","A=\"\n","=\"a\"\n","1A=\"a\"\n","A-B=\"a\"\n","A=\"a\"", "A=bare\n",
        "A=\"quote\"inside\"\n","A=\"back\\slash\"\n","A=\"cr\r\"\n","A=\"x\"\n\n"};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];++i) {
        char data[128];strcpy(data,invalid[i]);assert(compositor_environment(data,strlen(data)));
    }
    char embedded[]="A=\"x\"\n\0B=\"y\"\n";assert(compositor_environment(embedded,sizeof embedded-1));
    char many[1024]={0};for(unsigned i=0;i<65;++i) strcat(many,"A=\"x\"\n");assert(compositor_environment(many,strlen(many)));
    puts("UWSM_ENVIRONMENT boundedLiteralParsing=true malformedRejected=true shellEvaluation=false gpu=false");
    return 0;
}
