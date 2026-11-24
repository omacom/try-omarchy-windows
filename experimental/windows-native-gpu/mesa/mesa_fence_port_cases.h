/* CPU callbacks for the actual Mesa fence functions extracted by the runner.
 * No COM implementation, graphics library, native device, event or FD is used. */
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

using HANDLE = void *;
using HRESULT = int;
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
inline constexpr uint64_t OS_TIMEOUT_INFINITE = UINT64_MAX;
inline constexpr uint64_t NsPerMs = 1000000;
inline constexpr uint64_t MaxTimeoutInNs = static_cast<uint64_t>(UINT_MAX) * NsPerMs;
inline constexpr int PIPE_FD_TYPE_NATIVE_SYNC = 1;
inline constexpr unsigned long SYNC_IOC_FILE_INFO = 1;

struct ID3D12Fence {
   std::vector<uint64_t> values;
   unsigned reads = 0, registrations = 0;
   int set_result = 0;
   HANDLE last_event = reinterpret_cast<HANDLE>(1);
   uint64_t GetCompletedValue() {
      assert(!values.empty());
      unsigned i = reads++;
      return values[i < values.size() ? i : values.size() - 1];
   }
   HRESULT SetEventOnCompletion(uint64_t value, HANDLE event) {
      assert(value == 42);
      ++registrations;
      last_event = event;
      return set_result;
   }
};
struct d3d12_fence {
   int type = PIPE_FD_TYPE_NATIVE_SYNC;
   bool signaled = false;
   int sync_file_fd = -1;
   ID3D12Fence *cmdqueue_fence = nullptr;
   uint64_t value = 42;
   HANDLE event = nullptr;
   int event_fd = -1;
};
struct sync_file_info { int status = 0; };
bool create_ok = true, wait_ok = true;
int created = 0, closed = 0, waited = 0, sync_calls = 0, info_calls = 0;
int sync_result = 0, info_result = 0, info_status = 1, last_sync_timeout = 0;
HANDLE d3d12_fence_create_event(int *fd) {
   ++created;
   *fd = create_ok ? 17 : -1;
   return create_ok ? reinterpret_cast<HANDLE>(1) : nullptr;
}
void d3d12_fence_close_event(HANDLE, int) { ++closed; }
bool d3d12_fence_wait_event(HANDLE, int, uint64_t) { ++waited; return wait_ok; }
int sync_wait(int fd, int timeout) {
   assert(fd == 0);
   ++sync_calls; last_sync_timeout = timeout;
   return sync_result;
}
int ioctl(int fd, unsigned long request, sync_file_info *info) {
   assert(fd == 0 && request == SYNC_IOC_FILE_INFO);
   ++info_calls; info->status = info_status;
   return info_result;
}

// ACTUAL_MESA_FENCE_FUNCTIONS

void require(bool condition, const char *label) {
   if (!condition) throw std::runtime_error(label);
}
void reset() {
   create_ok = wait_ok = true;
   created = closed = waited = sync_calls = info_calls = 0;
   sync_result = info_result = last_sync_timeout = 0;
   info_status = 1;
}

int main() {
   try {
      unsigned cases = 0;
      auto run = [&](const char *name, std::vector<uint64_t> values, uint64_t timeout,
                     bool expected, int mode = 0) {
         reset();
         ID3D12Fence native;
         native.values = values;
         d3d12_fence fence;
         fence.cmdqueue_fence = &native;
         if (mode == 1) native.set_result = -1;
         if (mode == 2) create_ok = false;
         if (mode == 3) wait_ok = false;
         if (mode == 4) { fence.event = reinterpret_cast<HANDLE>(1); fence.event_fd = 17; }
         if (mode == 5) fence.signaled = true;
         require(d3d12_fence_finish(&fence, timeout) == expected, name);
         require(fence.signaled == expected, "Incorrect completion cache");
         if (timeout == 0 || (values[0] >= 42 && mode != 5) || mode == 5)
            require(created == 0 && native.registrations == 0 && waited == 0,
                    "Nonwaiting path created or waited for an event");
         if (mode == 1 && timeout != OS_TIMEOUT_INFINITE)
            require(closed == 1 && fence.event == nullptr && fence.event_fd == -1,
                    "Failed registration retained a lazy event");
         if (mode == 4) require(created == 0 && native.registrations == 0 && waited == 1,
                                "Existing event was registered again");
         if (mode == 5) require(native.reads == 0, "Cached completion queried the native fence");
         if (timeout == OS_TIMEOUT_INFINITE)
            require(created == 0 && waited == 0 && native.registrations == 1 &&
                    native.last_event == nullptr, "Infinite wait lost upstream blocking SEOC");
         ++cases;
      };
      run("Initial device removal must fail", {UINT64_MAX}, 0, false);
      run("Reached value", {42, 42}, 0, true);
      run("Device removal after observed completion", {42, UINT64_MAX}, 0, false);
      run("Zero timeout incomplete", {1}, 0, false);
      run("Lazy event completion", {1, 42}, 1, true);
      run("Wake without completion", {1, 2}, 1, false);
      run("Removal after wake", {1, UINT64_MAX}, 1, false);
      run("Registration failure", {1}, 1, false, 1);
      run("Event creation failure", {1}, 1, false, 2);
      run("Wait timeout", {1}, 1, false, 3);
      run("Existing lazy event", {1, 42}, 1, true, 4);
      run("Blocking SEOC completion", {1, 42}, OS_TIMEOUT_INFINITE, true);
      run("Blocking SEOC failure", {1}, OS_TIMEOUT_INFINITE, false, 1);
      run("Blocking SEOC removal", {1, UINT64_MAX}, OS_TIMEOUT_INFINITE, false);
      run("Cached completion", {0}, 0, true, 5);
      for (int mode = 0; mode < 5; ++mode) {
         reset();
         ID3D12Fence native; native.values = {UINT64_MAX};
         d3d12_fence fence; fence.cmdqueue_fence = &native; fence.sync_file_fd = 0;
         if (mode == 1) info_status = -5;
         if (mode == 2) info_result = -1;
         if (mode == 3) sync_result = -1;
         const uint64_t timeout = mode == 4 ? OS_TIMEOUT_INFINITE : 1;
         require(d3d12_fence_finish(&fence, timeout) == (mode == 0 || mode == 4),
                 "Native sync status/error handling");
         require(native.reads == 0 && created == 0 && sync_calls == 1,
                 "Native sync path fell through to raw fence");
         require(info_calls == (mode == 3 ? 0 : 1), "Native sync wait error was ignored");
         require(last_sync_timeout == (mode == 4 ? -1 : 1), "Timeout rounding/infinite conversion");
         ++cases;
      }
      std::printf("MESA_FENCE_PORT_CPU_PASS cases=%u gpuWorkloadRun=false\n", cases);
      return 0;
   } catch (const std::exception &error) {
      std::fprintf(stderr, "MESA_FENCE_PORT_CPU_FAIL %s\n", error.what());
      return 1;
   }
}
