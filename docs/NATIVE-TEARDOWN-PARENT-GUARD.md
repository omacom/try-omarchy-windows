# Preserve native teardown dependencies after failed release

Added November 5, 2026. Build and CPU-control evidence requires a completed CI
run for the current revision. These source changes do not establish hardware
recovery. Local GPU, VM, display and driver testing remain prohibited.

Disconnect cleanup previously attempted queue destruction, then continued
through allocations, contexts, devices, reservations and adapters regardless
of the result. Coordinated owner teardown retained failed child records but
still invoked their parents' destruction. The live-retirement path already
checked child dependencies; terminal cleanup lacked the same protection.

The new shared terminal cleanup routine:

- Removes an object only after an exact successful, empty destruction reply.
- Keeps later phases blocked while this session retains an earlier-phase object.
- Skips a parent while any recorded child remains, including within one phase.
- Allows a coordinated owner to retry the retained objects on a later pass.

Parent counts are built once per phase and updated after successful releases,
avoiding a whole-table scan per object. If that bounded index cannot be allocated,
the phase retains ownership and performs no native destruction.

Failed queues block the allocation phase because their private data can refer
to allocations outside the direct parent chain. A failed allocation blocks its
device; a failed device blocks its adapter. An EOF session applies the same
checks before its wire bookkeeping disappears. Native ownership maps and failed
cleanup reporting remain the backstop; this does not establish leak-free recovery.

The native driver also refuses device destruction while its global ownership
maps contain a context, hardware/paging queue, sync object, standard allocation
or vendor allocation on that device. Adapter close refuses a tracked device.
Informational positive native replies for device/adapter destruction preserve
ownership and become a failure. These checks apply across coordinated actors,
independently of one session's records.

Microsoft documents [D3DKMTDestroyDevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtdestroydevice)
as releasing a kernel device context. The dependency checks here are this
project's ownership invariant; no implicit cleanup behavior is assumed.

The proposed CPU regression uses the actual `Session` implementation and a
mock `Driver`. Its graph contains adapter, device, context, hardware queue,
borrowed queue sync and standard allocation. It defines 26 cases: EOF and
coordinated cleanup, each with a healthy case and twelve faults. Queue,
allocation and device releases receive negative, informational, nonempty-handle
and nonempty-value replies. The controls reject parent calls after those faults,
check retained ownership, and retry coordinated cleanup after removing the fault.

Remote CI builds that same fixture against the pinned pre-fix `0a2dea0` header
and the candidate header. The baseline must fail with the targeted dependency
marker, not a compiler error; the candidate must pass all 26 controls and the
existing wire suite. Comparison subprocesses have deadlines, owned process
groups and disabled core dumps. Logs contain mock protocol data only.

Windows CI also compiles the native guard changes and runs the wire controls.
No new control creates a Windows graphics device, calls KMT, starts a VM or
runs a renderer. No results are claimed before CI completes. These checks do
not prove native driver cleanup, GPU retirement or recovery after the display
incident, and the incident's cause remains unverified.

Normal launcher sessions, preserved official input routing in operation,
responsive applications and browsing through 4K, sustained resource bounds,
CUDA/OpenGL/Vulkan/video/NVIDIA features and matched-native performance still
require broader implementation and validation. Counts stay **4/8, 0/4, 3/3**.
