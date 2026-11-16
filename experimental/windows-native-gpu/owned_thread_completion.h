// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <thread>

namespace driver_shared {
enum class ThreadCompletion { Pending, Complete, Failed };
// A zero-time query of the held thread handle, including thread/TLS teardown.
// Never join a live graphics thread from the controller's RPC dispatcher.
inline ThreadCompletion queryOwnedThreadCompletion(std::thread& thread) {
    if (!thread.joinable()) return ThreadCompletion::Complete;
    const auto state = WaitForSingleObject(thread.native_handle(), 0);
    if (state == WAIT_TIMEOUT) return ThreadCompletion::Pending;
    if (state == WAIT_OBJECT_0) return ThreadCompletion::Complete;
    return ThreadCompletion::Failed;
}
}
