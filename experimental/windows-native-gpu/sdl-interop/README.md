# SDL/D3D12 host interop control

Build with Visual Studio C++ tools and the Windows SDK:

    .\build.ps1 -OutputDirectory C:\path\to\fresh-build

Run the resulting executable with an absolute path to the pinned SDL2 DLL and
a fresh absolute output directory:

    C:\path\to\fresh-build\sdl-d3d12-interop.exe C:\path\to\qemu\bin\SDL2.dll C:\path\to\fresh-captures

The default mode performs two bounded 4K GPU texture transfers to a hidden SDL
OpenGL backbuffer, with separate named producer/consumer fences and full
diagnostic readback. The optional --capabilities-only argument initializes and
queries the context without exercising texture transfer; that mode cannot pass
the full auditor. The process requires NVIDIA hardware with matching GL/D3D12
adapter LUID and the EXT Win32 external-memory/semaphore extensions. It does
not install drivers, change input routing or select a launcher runtime.

The --shared-contexts mode imports the shared texture and semaphores in a
separate shared OpenGL 4.5 core context, then creates the non-shareable FBO and
performs GPU waits, readback, blit and consumer signaling in the original SDL
root context. This exercises the context boundary needed by QEMU; the producer
and consumer still run in one host process.

The strict MSVC build uses only Windows SDK libraries and the minimal Khronos
extension ABI header included here. It loads the supplied SDL DLL dynamically;
no SDL development SDK or external GL header download is required.

See [actual results, ownership contract and limits](../../../docs/LIVE-SDL-D3D12-INTEROP.md).
The public milestone tests replay the frozen captures and reject stale/corrupt
pixels, failed fences, different builds/adapters, missing cleanup and broader
guest/display/performance claims.
