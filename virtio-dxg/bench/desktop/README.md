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
Enter to the app's window (`openwindow`), closes the window and stops what
the launch started. Round 1 is the first launch after boot.

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

The first PR #5 checks at `28b7f55` passed the Mesa build, host mock calls,
guest contract and Windows and Linux launcher checks. The locked Linux
7.3.5 module build stopped downloading headers before compilation. Four
historical replay jobs also lacked their archived fixtures. These are build
and regression results. Desktop timings below are from the earlier build.

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
