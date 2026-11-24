// SPDX-License-Identifier: MIT
// The held worker only waits on a kernel event; no graphics, windows or input.
#include "owned_thread_completion.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace driver_shared;
    std::thread empty;
    assert(queryOwnedThreadCompletion(empty)==ThreadCompletion::Complete);
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    assert(release);
    std::thread worker([&] { assert(WaitForSingleObject(release,10000)==WAIT_OBJECT_0); });
    for(unsigned n=0;n<1000;++n)
        assert(queryOwnedThreadCompletion(worker)==ThreadCompletion::Pending);
    assert(worker.joinable());
    assert(SetEvent(release));
    assert(WaitForSingleObject(worker.native_handle(),1000)==WAIT_OBJECT_0);
    assert(queryOwnedThreadCompletion(worker)==ThreadCompletion::Complete);
    worker.join();
    assert(queryOwnedThreadCompletion(worker)==ThreadCompletion::Complete);
    assert(CloseHandle(release));
    std::puts("OWNED_THREAD_COMPLETION_PASS pendingRetained=1000 heldHandle=true joinAfterTermination=true gpu=false");
}
