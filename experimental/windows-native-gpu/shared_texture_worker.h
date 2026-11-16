// SPDX-License-Identifier: MIT
// Trusted coordinator -> owned Windows GPU process, over private inherited
// pipes. The guest never chooses a PID, HANDLE, executable or channel.
#pragma once
#include "native_worker_protocol.h"
#include "shared_texture_consumer.h"
#include "shared_resource_handle.h"
#include <cerrno>
#include <cstdlib>
#include <limits>
namespace driver_worker {
class Handle {
    HANDLE value=nullptr;
public:
    Handle()=default;
    explicit Handle(HANDLE h):value(h) {}
    ~Handle() { if (get()) CloseHandle(value); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this!=&other) { if (get()) CloseHandle(value); value=std::exchange(other.value,nullptr); } return *this;
    }
    HANDLE get() const { return value==INVALID_HANDLE_VALUE ? nullptr : value; }
    HANDLE release() { return std::exchange(value,nullptr); }
};
struct Counters { unsigned started=0,exited=0,forced=0,failed=0,transferred=0; };
inline std::uintptr_t number(const char* text) {
    if (!text || !*text || std::strspn(text,"0123456789")!=std::strlen(text)) throw std::runtime_error("Invalid private worker handle");
    errno=0; char* end=nullptr; const auto value=std::strtoull(text,&end,10);
    if (errno || !end || *end || !value || value>=std::numeric_limits<std::uintptr_t>::max()) throw std::runtime_error("Private worker handle overflow");
    return static_cast<std::uintptr_t>(value);
}
inline int run(const char* inputText,const char* outputText) {
    Handle input(reinterpret_cast<HANDLE>(number(inputText))),output(reinterpret_cast<HANDLE>(number(outputText)));
    if (input.get()==output.get() || GetFileType(input.get())!=FILE_TYPE_PIPE || GetFileType(output.get())!=FILE_TYPE_PIPE)
        throw std::runtime_error("Private worker requires distinct pipe handles");
    std::unique_ptr<driver_shared::TextureConsumer> consumer;
    std::uint64_t next=1;
    for (;;) {
        Request request{}; DWORD count=0;
        if (!ReadFile(input.get(),&request,sizeof(request),&count,nullptr) || count!=sizeof(request) || !valid(request,next,consumer!=nullptr))
            throw std::runtime_error("Private worker request rejected");
        Response reply{}; reply.operation=request.operation; reply.sequence=request.sequence; reply.workerPid=GetCurrentProcessId();
        if (request.operation==Operation::Initialize) {
            consumer=std::make_unique<driver_shared::TextureConsumer>();
            const LUID luid{request.luidLow,request.luidHigh};
            reply.hresult=consumer->initializeOwnedHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(request.ownedNtHandle)),luid,(request.flags&Present)!=0);
            std::cerr<<"{\"nativeGpuWorkerInitialized\":true,\"pid\":"<<reply.workerPid<<",\"parentPidChosenByGuest\":false,\"hresult\":"<<reply.hresult<<",\"ntHandleClosedAfterOpen\":true}\n";
        } else if (request.operation==Operation::Consume) {
            reply.hresult=consumer->consume(reply.hash,(request.flags&Streaming)!=0);
            if (reply.hresult<0) reply.hash=0;
            std::cerr<<"{\"nativeGpuWorkerConsumed\":true,\"pid\":"<<reply.workerPid<<",\"sequence\":"<<request.sequence<<",\"hresult\":"<<reply.hresult<<",\"fnv1a\":"<<reply.hash<<"}\n";
        } else {
            consumer.reset();
            std::cerr<<"{\"nativeGpuWorkerShutdown\":true,\"pid\":"<<reply.workerPid<<",\"resourcesReleased\":true}\n";
        }
        count=0;
        if (!WriteFile(output.get(),&reply,sizeof(reply),&count,nullptr) || count!=sizeof(reply)) throw std::runtime_error("Private worker reply failed");
        if (reply.hresult<0) return 1;
        if (request.operation==Operation::Stop) return 0;
        if (++next==0) throw std::runtime_error("Private worker sequence exhausted");
    }
}
class TextureWorker {
    Counters& counters;
    Handle process,job,input,output;
    DWORD pid=0;
    std::uint64_t sequence=0;
    bool initialized=false,failed=false,retired=false,forced=false;
    void failure() { if (!failed) { failed=true; ++counters.failed; } }
    Response exchange(Request request) {
        if (!process.get() || failed || retired || sequence==UINT64_MAX || WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT)
            throw std::runtime_error("Owned GPU worker unavailable");
        request.sequence=++sequence;
        DWORD written=0;
        // At most one 40-byte request is outstanding. It fits the private
        // pipe buffer even if the child stops reading; responses have a deadline.
        if (!WriteFile(input.get(),&request,sizeof(request),&written,nullptr) || written!=sizeof(request)) throw std::runtime_error("Owned GPU worker send failed");
        const auto deadline=GetTickCount64()+10000;
        for (;;) {
            DWORD available=0;
            if (!PeekNamedPipe(output.get(),nullptr,0,nullptr,&available,nullptr)) throw std::runtime_error("Owned GPU worker reply pipe failed");
            if (available>=sizeof(Response)) break;
            if (GetTickCount64()>=deadline || WaitForSingleObject(process.get(),1)!=WAIT_TIMEOUT) throw std::runtime_error("Owned GPU worker reply deadline or exit");
        }
        Response reply{}; DWORD read=0;
        if (!ReadFile(output.get(),&reply,sizeof(reply),&read,nullptr) || read!=sizeof(reply) || !valid(reply,request,pid))
            throw std::runtime_error("Owned GPU worker reply identity mismatch");
        return reply;
    }
    void launch() {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        HANDLE read=nullptr,write=nullptr;
        if (!CreatePipe(&read,&write,&security,4096)) throw std::runtime_error("Worker command pipe creation failed");
        Handle childInput(read); input=Handle(write);
        if (!SetHandleInformation(input.get(),HANDLE_FLAG_INHERIT,0)) throw std::runtime_error("Worker parent pipe flags failed");
        if (!CreatePipe(&read,&write,&security,4096)) throw std::runtime_error("Worker response pipe creation failed");
        output=Handle(read); Handle childOutput(write);
        if (!SetHandleInformation(output.get(),HANDLE_FLAG_INHERIT,0)) throw std::runtime_error("Worker response flags failed");
        Handle null(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
        HANDLE error=nullptr;
        if (!null.get() || !DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_ERROR_HANDLE),GetCurrentProcess(),&error,0,TRUE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Worker standard handles failed");
        Handle childError(error);
        job=Handle(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.get() || !SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("Worker job creation failed");
        HANDLE inherited[]{childInput.get(),childOutput.get(),childError.get(),null.get()};
        SIZE_T bytes=0; InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        struct Attributes {
            LPPROC_THREAD_ATTRIBUTE_LIST value=nullptr; bool initialized=false;
            ~Attributes() { if (initialized) DeleteProcThreadAttributeList(value); if (value) HeapFree(GetProcessHeap(),0,value); }
        } attributes;
        attributes.value=static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(),0,bytes));
        attributes.initialized=attributes.value && InitializeProcThreadAttributeList(attributes.value,1,0,&bytes);
        if (!attributes.initialized || !UpdateProcThreadAttribute(attributes.value,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))
            throw std::runtime_error("Worker explicit inheritance failed");
        std::array<wchar_t,32768> executable{};
        const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        if (!length || length>=executable.size()) throw std::runtime_error("Worker executable identity unavailable");
        std::wstring command=L"\""+std::wstring(executable.data())+L"\" --native-texture-worker "+
            std::to_wstring(reinterpret_cast<std::uintptr_t>(childInput.get()))+L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(childOutput.get()));
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=null.get(); startup.StartupInfo.hStdOutput=null.get(); startup.StartupInfo.hStdError=childError.get(); startup.lpAttributeList=attributes.value;
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&child))
            throw std::runtime_error("Owned GPU worker creation failed");
        process=Handle(child.hProcess); Handle thread(child.hThread); pid=child.dwProcessId; ++counters.started;
        if (!AssignProcessToJobObject(job.get(),process.get()) || ResumeThread(thread.get())==static_cast<DWORD>(-1)) throw std::runtime_error("Owned GPU worker start failed");
        std::cerr<<"{\"nativeGpuWorkerStarted\":true,\"pid\":"<<pid<<",\"coordinatorPid\":"<<GetCurrentProcessId()<<",\"privatePipes\":true,\"explicitInheritance\":true,\"guestChoosesHandle\":false}\n";
    }
public:
    explicit TextureWorker(Counters& value):counters(value) {}
    TextureWorker(const TextureWorker&)=delete;
    ~TextureWorker() { (void)stop(); }
    HRESULT initialize(D3DKMT_HANDLE resource,LUID luid,bool present=false) {
        try {
            if (process.get()) return E_UNEXPECTED;
            launch();
            driver_shared::ResourceHandle shared;
            const auto status=shared.share(resource);
            if (status) { failure(); return E_FAIL; }
            HANDLE remote=nullptr;
            if (!DuplicateHandle(GetCurrentProcess(),shared.get(),process.get(),&remote,0,FALSE,DUPLICATE_SAME_ACCESS)) { failure(); return E_FAIL; }
            ++counters.transferred;
            Request request{}; request.ownedNtHandle=reinterpret_cast<std::uintptr_t>(remote); request.luidLow=luid.LowPart; request.luidHigh=luid.HighPart; request.flags=present?Present:0;
            // A failed initialization kills/reaps this exact owned worker;
            // its duplicated NT reference cannot escape into another process.
            const auto reply=exchange(request);
            if (reply.hresult<0) { failure(); return reply.hresult; }
            initialized=true; return S_OK;
        } catch (const std::exception& error) { failure(); std::cerr<<"{\"nativeGpuWorkerFailed\":true,\"stage\":\"initialize\"}\n"<<error.what()<<'\n'; return E_FAIL; }
    }
    HRESULT consume(std::uint64_t& hash,bool streaming=false) {
        hash=0;
        try {
            if (!initialized) return E_UNEXPECTED;
            Request request{}; request.operation=Operation::Consume; request.flags=streaming?Streaming:0;
            const auto reply=exchange(request);
            if (reply.hresult<0) { failure(); return reply.hresult; }
            hash=reply.hash; return S_OK;
        } catch (const std::exception& error) { failure(); std::cerr<<"{\"nativeGpuWorkerFailed\":true,\"stage\":\"consume\"}\n"<<error.what()<<'\n'; return E_FAIL; }
    }
    bool stop() noexcept {
        if (!process.get() || retired) return !failed;
        if (initialized && !failed) {
            try { Request request{}; request.operation=Operation::Stop; if (exchange(request).hresult) failure(); }
            catch (...) { failure(); }
        }
        // A worker that has not accepted initialization must be able to exit
        // on EOF. Waiting with its input open would force-kill an idle child.
        if (!initialized || failed) input=Handle();
        if (WaitForSingleObject(process.get(),5000)!=WAIT_OBJECT_0) {
            failure(); forced=true; ++counters.forced;
            if (!TerminateProcess(process.get(),3) || WaitForSingleObject(process.get(),5000)!=WAIT_OBJECT_0) {
                // Preserve the job/process owners when exit cannot be proven.
                // The surrounding native session reports failed cleanup.
                (void)job.release(); (void)process.release(); return false;
            }
        }
        DWORD code=0; if (!GetExitCodeProcess(process.get(),&code) || code) failure();
        retired=true; ++counters.exited;
        std::cerr<<"{\"nativeGpuWorkerExited\":true,\"pid\":"<<pid<<",\"exit\":"<<code<<",\"forced\":"<<(forced?"true":"false")<<",\"failed\":"<<(failed?"true":"false")<<"}\n";
        process=Handle(); input=Handle(); output=Handle(); job=Handle(); return !failed;
    }
};
}
