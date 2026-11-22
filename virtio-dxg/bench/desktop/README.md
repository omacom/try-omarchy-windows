# Desktop responsiveness checks

Measurements of what a user feels on the virtio-dxg desktop, run inside the
guest's Hyprland session (`WAYLAND_DISPLAY` and `HYPRLAND_INSTANCE_SIGNATURE`
set).

## Frame pacing under load

For a single active guest output, `deskstat.py` reports the rate across the
entire marked phase, including startup and intervals of 100 ms or longer.
The estimated shortfall compares delivered events with the requested refresh
rate. Guest page flips and browser callbacks each need a matching host
presentation trace before claiming sustained 240 Hz on the Windows display.
The historical table below used the older calculation.

The load driver times each phase after a one-second warm-up, checks that the
scene and its load processes stay alive, and excludes transitions between
phases. Discard the trace if the driver exits with an error.

### Files scrolling

`navscroll.py` opens Files on a folder of 3,000 files, moves the pointer
over the grid (from elsewhere first: a window that opens under a resting
pointer gets no pointer focus) and scrolls with wheel notches through
QEMU's input, every `NAVSCROLL_INTERVAL_MS` (25 by default). It counts the
window's commits from `WAYLAND_DEBUG`: frames per second and the frame
gaps. With `NAVSCROLL_SHOTS=DIR` it saves window captures during the
scroll; `iconcheck.py DIR/*.png` (on the host, with PIL) finds icons that
differ from the folder's common icon. Blurred icons (sub-pixel positions
mid-scroll) and a notification over the grid show up too; look at the
flagged cells before calling them corrupted. `iconruns.sh` repeats runs.
Omarchy's notification layer takes pointer input over the right part of
the screen while a notification shows, so the pointer goes left of it.

| Files, 3,000 entries | Notches every 25 ms | every 8 ms | Corrupted icons |
| --- | --- | --- | --- |
| Before (d3d12 with threaded context, sync destroys) | 24–26 fps | 21–29 fps | rare (wrong scale or offset) |
| Texture recycling, no threaded context, async destroys, polling lanes | 54–56 fps | 70–97 fps | none in thousands |

Where a frame went before: 78 offscreen textures per frame (GTK offscreens
clipped grid items), each a CreateAllocation, MapGpuVirtualAddress and
DestroyAllocation on the host (~20 ms per frame), lane threads woken from
sleep for each (a CreateAllocation blocked 6.5 ms), and destroys waited for.

Then, in a larger window (second VM, Files 1896x1013, notches every 8 ms,
kernel 7.3.5, Mesa 26.3.1):

| Files, 3,000 entries, 1896x1013 | Frame rate | Corrupted icons |
| --- | --- | --- |
| Texture recycling, no threaded context | 41–42 fps | |
| Constant-buffer views staged in a CPU heap | 54–65 fps | 0 of 879 |
| Plus texture-cache buckets, budget read every 50 ms | 61–69 fps | 0 of 614 |

17% of Files' main thread was NVIDIA's `CreateConstantBufferView` reading
back each descriptor it had just written into the shader-visible heap,
GPU memory the guest maps write-combined: microseconds per read.
A `perf record` while `navscroll.py` ran found it; a breakpoint on the hot instruction showed the 32-byte steps through
the 256 KB heap. Mesa now builds those views in a CPU-only heap and copies
them, as it already did for textures. `QMP_PORT` (default 4444) points
`navscroll.py` at another VM's QMP, `NAVSCROLL_SECS` sets the scroll
length, and `iconcheck.py` finds the icon columns itself.

What is left per frame (off-CPU profile of Files' main thread): GTK's own
work (layout, text shaping of items scrolling in, about 60% of its CPU),
waits for the GPU at each window-buffer flush, CreateAllocation replies
that sometimes arrive milliseconds late, and short GObject lock waits
against Files' worker threads. From the Wayland trace, a frame is about
8.8 ms in Files (frame callback to commit) and 3.1 ms in the compositor
(commit to the next callback), medians.

Tried and dropped, each measured with alternating runs: batching
asynchronous kicks (`kick_delay_us`, slower), keeping vCPUs from halting
(idle spinners: much slower, they take host cores from QEMU's threads),
holding the GPU at higher clocks (it idles at ~700 MHz during the scroll,
but a background load that raised it changed nothing), GTK's Vulkan
renderer on Dozen (slower than `GSK_RENDERER=gl`).

`NAVSCROLL_HIRES=V` scrolls with high-resolution wheel steps of V/120
notch (QMP `rel` events on the `wheel` axis, as a touchpad or free-spinning
wheel sends them through QEMU's window) instead of whole notches.

In a disposable guest session, open Files on the same 3,000-entry directory
for each build. Keep the viewport, icon theme, scale, sort mode, thumbnail
cache and scroll sequence identical. Record the Nautilus process ID. During
a 15-second scroll, run `sudo bpftrace nautilus-ioctl.bt PID` inside the guest.
It groups DXG ioctls from all Nautilus threads by command number and reports
count, summed elapsed microseconds and worst call. Match command numbers to
the second argument of `_IOWR(0x47, ...)` in the guest's `d3dkmthk.h`.
Overlapping waits can sum to more than wall time. Compare a second untraced
run for pacing. Also check icon pixels, thumbnail updates, selection and
menu animations. No new measurements are available for this candidate.

`files-scroll.py DIRECTORY OUTPUT` opens Files through the menu on an empty
guest workspace, navigates to DIRECTORY, places the guest pointer over the
file grid and sends wheel events for 15 seconds. Use a lowercase path and a
fresh output directory. It records the guest mode, window size, input times
and two window captures. Check that scrolling revealed different files and
that icons and thumbnails rendered correctly. A change in PNG bytes alone
cannot establish either. Analyze the marked interval with `deskstat.py`.
That rate counts compositor flips. To compare against the reported 22–26
Files fps, also record Nautilus frame timings with the same client-side
instrumentation on both runs. An output can flip between two Files updates.
GTK can export its frame-clock phases through Sysprof. Use a separate
profiled run to identify where Files spends time and keep the unprofiled
run for the launcher and output timings.
[GTK profiling](https://docs.gtk.org/gtk4/running.html#profiling).

For a transport-only comparison, use the same guest image, boot hook, Mesa
archive and QEMU runtime for both runs. Change only the DXG module. The
performance branch workflow builds the reference from `fa134286` alongside
the candidate using the same signed headers and compiler. For first-launch
timings, boot fresh disposable overlays at the same point after login and
report whether idle prefetch and the browser service have finished.

A window that animates every frame stays on screen while the guest is idle,
while every core is busy (stress-ng), while the GPU is busy (a heavy
off-screen glmark2 scene) and both. The kernel's page-flip events show
whether the compositor met each vblank.

```sh
sudo bash deskbench.sh start            # page flips into the trace buffer
bash deskload.sh 15                     # as the desktop user: 4 phases of 15 s
sudo OUT=/tmp bash deskbench.sh stop desk
python3 deskstat.py /tmp/desk.trace 240 # flips/s and missed vblanks per phase
```

Gaps that repeat every 10 s are glmark2 restarting its scene, not the
desktop.

## Launching from the app menu

`launchertest.py` opens every application the launcher lists the way a user
does: SUPER+SPACE (Omarchy's menu), the app's name, Enter. The key presses go
in through QEMU's input devices (QMP, reached from the guest at the user-mode
network's host address, `10.0.2.2:4444`), so they take the guest's whole
input path. It reports the menu's appearance (Hyprland's `openlayer`) and
Enter to the app's window (`openwindow`). It times and closes a new window
on workspace 7 only when its class uniquely matches the requested desktop
entry. Apps sharing a window class can produce an “unverified window”
result. A missing window, including an app that focuses an existing
instance, has no launch-time sample. Use a disposable guest. Round 1 counts
as a first launch only for apps that have not run since boot.

```sh
python3 launchertest.py 2               # all apps, two rounds
python3 launchertest.py 2 youtube,files # some
python3 menuprobe.py                    # the menu's events after SUPER+SPACE
```

`webapp.py` times a web app window (`--app=URL`, as Omarchy's web apps)
from launch to the window and to the page's first and largest contentful
paint, over the DevTools protocol; `pageload.py` does the same for a page
in a fresh profile. Both also run on Windows (Chrome, Edge) for comparison.

## Results

At `8381c1d`, the signed-header build produced DXG and both legacy modules
for Linux `7.3.5-arch1-1`. Mesa 26.2.6, the host mocks, guest contract,
launcher and replay checks passed. The separate full-image build stopped
before boot when the package mirror diverged from the lock. The upstream
image built with the same guest sources had already passed its boot check.
The table below records the earlier desktop build.
[Module and Mesa checks](https://github.com/7Wdev/try-omarchy-windows/actions/runs/44955823350) ·
[Full-image run](https://github.com/7Wdev/try-omarchy-windows/actions/runs/44956203097) ·
[Upstream image](https://github.com/omacom/try-omarchy-windows/actions/runs/44854721042).

RTX 5090 Laptop GPU, 16 vCPUs, 240 Hz window. Before and after the boot
hook's desktop priority, GTK's OpenGL renderer, app prefetch and browser
boost, and the window-buffer leak fix.

Frame pacing (6 vCPUs):

| | Before | After |
| --- | --- | --- |
| Missed vblanks per 15 s: idle / all cores busy | 7–148 / 14–177 | 5 / 8 |
| Missed vblanks per 15 s: GPU busy / CPU and GPU busy | 7–12 / 3–4 | 15 / 2 |

Enter in the menu to the window, first launch after boot / warm:

| Application | Before | After |
| --- | --- | --- |
| Web apps (YouTube, WhatsApp, Discord, X, Google, HEY, Zoom, Basecamp) | 1030–1340 ms | 366–448 ms |
| Chromium | about 1 s | 369 / 381 ms |
| Visual Studio Code | 5688 / 1515 ms | 1851 / 1517 ms |
| disktree | 3878 / 754 ms | 975 / 579 ms |
| mpv | 1838 / 981 ms | 1093 / 985 ms |
| Omacalc, Omawrite | 509–811 ms | 442–532 ms |
| Files (Nautilus) | 524–532 ms | 615–687 ms |
| Foot, Neovim | 117–129 ms | 92–130 ms |

The menu itself appears 130–210 ms after SUPER+SPACE.

YouTube as a web app, from launch, warm profile:

| | Window | Largest contentful paint |
| --- | --- | --- |
| Before | 940–1400 ms | 1600–1770 ms |
| Browser boost | 330–370 ms | 875–990 ms |
| Windows 11 host, Chrome (starting) | – | 1000–1130 ms |
