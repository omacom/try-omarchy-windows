# virtio-dxg benchmarks

[Browser frame pacing](browser-pacing.html) is an offline page for a visible
guest Chromium, Firefox or Electron window. Select the measured monitor rate
and run at each window size and zoom level. Record the application build, GPU
diagnostics, four corner markers and drawing-buffer dimensions with the JSON
result. Browser callbacks measure scheduling inside the client. Use a separate
host presentation trace to count delivered frames. Check browser decorations,
close, zoom out and reset through the normal desktop input path.

The scripts behind the numbers in [docs/VIRTIO-DXG-RESULTS.md](../../docs/VIRTIO-DXG-RESULTS.md).
They run inside a development VM (`guest/test/initcpio`, kernel arguments
`virtio_dxg.desktop=1 virtio_dxg.root=1`). That VM mounts a host folder at
`/mnt/vdxgshare` holding these scripts and the test programs built from
`guest/test` (`cudatest`, `d3d12test`).

| Script | Measures |
| --- | --- |
| `suite.sh [quick]` | Everything once: OpenGL (glmark2), Vulkan (vkmark, vkcube), CUDA, video decode and encode, image decode and scaling, the NVIDIA libraries, D3D12 features, the desktop's vsync rate. |
| `video-bench.sh` | NVDEC, NVENC H.264/HEVC/AV1 and a transcode, 1200 frames each, start-up included. Also runs on Windows (Git Bash, `FFMPEG=ffmpeg.exe`) for the native baseline. |
| `steady.sh` | Start-up and steady-state rates apart (60 frames versus 1200). |
| `video-stress.sh` | Video sessions back to back (`ROUNDS`, `PAUSE`); reports failures. |
| `fifo.sh [frames]` | vkcube with vsync: the frame rate a client gets from the compositor. |
| `desk-stress.sh [seconds]` | Hyprland's debug overlay, vkcube and glmark2 together; checks the compositor answers every 2 s. |
| `awake.sh` | Keeps the display on (an idle session turns it off and throttles every client). |

With `DXGHOST_STATS=1` in QEMU's environment, QEMU reports per-command host
time and, with zero-copy scanout, the presented frame rate every 10 s:
`scanout 238.9 fps over 10 s (display 240 Hz): 3 gaps over 1.5 frames, worst
9.1 ms`.
