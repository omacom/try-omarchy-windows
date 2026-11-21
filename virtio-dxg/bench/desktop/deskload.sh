#!/bin/bash
# deskload.sh [SECONDS]: a window that animates every frame (glmark2-wayland,
# a light scene, vsync) stays on screen through four phases marked in the
# trace (deskbench.sh records page flips): idle, all cores busy, the GPU busy
# (a heavy off-screen render), both. As the desktop user, in a test VM.
# Run in the desktop session (WAYLAND_DISPLAY, HYPRLAND_INSTANCE_SIGNATURE set).
T=/sys/kernel/tracing/trace_marker
S=${1:-10}
mark() { echo "$1" > $T; }
cpu() { stress-ng --cpu "$(nproc)" --cpu-method matrixprod --timeout $((S + 2)) >/dev/null 2>&1 & }
gpu() { timeout $((S + 2)) glmark2 --off-screen -s 2560x1440 --run-forever -b terrain >/dev/null 2>&1 & }
# Keep the session from idling or locking during the run (Omarchy's
# stay-awake toggle), or the display stops presenting.
hyprctl eval "hl.dispatch(hl.dsp.focus({ workspace = 9 }))" >/dev/null 2>&1
setsid -f timeout $((4 * S + 20)) glmark2-wayland -s 800x600 -b build --run-forever >/dev/null 2>&1 < /dev/null
sleep 3
mark idle; sleep $S
mark cpu; cpu; sleep 1; sleep $S; wait
mark gpu; gpu; sleep 1; sleep $S; wait
mark cpu+gpu; cpu; gpu; sleep 1; sleep $S; wait
mark end
pkill -x glmark2-wayland
hyprctl eval "hl.dispatch(hl.dsp.focus({ workspace = 1 }))" >/dev/null 2>&1
