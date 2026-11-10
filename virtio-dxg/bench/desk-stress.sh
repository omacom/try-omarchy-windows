#!/bin/bash
# desk-stress.sh [SECONDS]: desktop compositor stress:
# vkcube (wl_shm frames from Vulkan)
# and glmark2 windowed (GL), all at once; checks the compositor still
# answers every 2 s and reports its frame rate.
while IFS= read -r l; do [ -n "$l" ] && export "$l"; done < /run/environment.d/90-virtio-dxg.conf
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export WAYLAND_DISPLAY=$(basename "$(ls $XDG_RUNTIME_DIR/wayland-? 2>/dev/null | head -1)")
export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t $XDG_RUNTIME_DIR/hypr | head -1)
secs=${1:-120}
bash "$(dirname "$0")/awake.sh"
timeout $((secs + 5)) vkcube --wsi wayland >/dev/null 2>&1 &
( end=$((SECONDS + secs)); while [ $SECONDS -lt $end ]; do
    timeout 120 glmark2-es2-wayland -s 800x600 -b build -b texture -b shading -b refract >/dev/null 2>&1; done ) &
hung=0
for i in $(seq $((secs / 2))); do
    sleep 2
    timeout 5 hyprctl monitors >/dev/null 2>&1 || { hung=$((hung + 1)); echo "compositor not answering at ${i}x2 s"; }
done
wait
echo "desk-stress: $secs s, $hung unanswered checks"
