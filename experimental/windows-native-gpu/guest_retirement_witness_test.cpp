// SPDX-License-Identifier: MIT
// Actual witness file operations in an owned Linux temporary directory; no GPU.
#include "guest_coordinator.h"
#include <dirent.h>
#include <iostream>
using namespace guest_broker;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(0)
static unsigned descriptors() {
    DIR* directory=opendir("/proc/self/fd");CHECK(directory);unsigned count=0;
    for(dirent* entry;(entry=readdir(directory));) if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    CHECK(!closedir(directory));return count;
}
int main() {
    CHECK(!getuid() && !geteuid());
    char pattern[]="/tmp/omarchy-retirement-witness-XXXXXX";
    const char* folder=mkdtemp(pattern);CHECK(folder);
    const std::string directory=folder,missing=directory+"/not-created",name=directory+"/"+std::to_string(getpid());
    const auto before=descriptors();
    for(std::uint64_t actor=1;actor<=10000;++actor) publishRetirementWitness(false,getpid(),actor,missing.c_str());
    struct stat state{};CHECK(lstat(missing.c_str(),&state) && errno==ENOENT && descriptors()==before);
    publishRetirementWitness(true,getpid(),77,directory.c_str());
    CHECK(!lstat(name.c_str(),&state) && S_ISREG(state.st_mode) && !state.st_uid && state.st_nlink==1 && (state.st_mode&0777)==0444);
    auto contents=[&] {
        Fd file(open(name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));CHECK(file.get()>=0);char bytes[32]{};
        const auto count=read(file.get(),bytes,sizeof bytes);CHECK(count==3 && !memcmp(bytes,"77\n",3));
    };
    contents();bool collision=false;
    try { publishRetirementWitness(true,getpid(),78,directory.c_str()); } catch(const std::runtime_error&) { collision=true; }
    CHECK(collision);contents();
    for(std::uint64_t actor=78;actor<10078;++actor) publishRetirementWitness(false,getpid(),actor,directory.c_str());
    contents();CHECK(descriptors()==before);
    const auto alias=directory+"/alias";CHECK(!symlink(directory.c_str(),alias.c_str()));
    bool rejected=false;try { publishRetirementWitness(true,getpid(),79,alias.c_str()); } catch(const std::runtime_error&) { rejected=true; }
    CHECK(rejected && !unlink(alias.c_str()));
    CHECK(!chmod(directory.c_str(),0775));rejected=false;
    try { publishRetirementWitness(true,getpid(),79,directory.c_str()); } catch(const std::runtime_error&) { rejected=true; }
    CHECK(rejected && !chmod(directory.c_str(),0700));
    CHECK(!unlink(name.c_str()) && !symlink("/dev/null",name.c_str()));rejected=false;
    try { publishRetirementWitness(true,getpid(),79,directory.c_str()); } catch(const std::runtime_error&) { rejected=true; }
    CHECK(rejected && !lstat(name.c_str(),&state) && S_ISLNK(state.st_mode));
    CHECK(!unlink(name.c_str()) && !rmdir(directory.c_str()) && descriptors()==before);
    std::cout<<"RETIREMENT_WITNESS_CONTROL samePidActors=20000 normalFilesCreated=0 legacyWitnessPreserved=true legacyCollisionRejected=true unsafeDirectoryRejected=true symlinkRejected=true descriptorsBalanced=true gpu=false vm=false\n";
}
